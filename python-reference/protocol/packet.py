"""
Кодек пакета телеметричного протоколу.

Формат пакета (мережевий порядок байтів, big-endian):

+----------+------+---------------------------------------------+
| Поле     | Байт | Опис                                          |
+----------+------+---------------------------------------------+
| version  |  1   | Версія протоколу (unsigned char)             |
| msg_type |  1   | Тип повідомлення, MessageType (unsigned char)|
| node_id  |  2   | Ідентифікатор вузла (unsigned short)         |
| sequence |  4   | Порядковий номер пакета (unsigned int)       |
| timestamp|  8   | Час відправлення, мс (unsigned long long)    |
| pay_len  |  2   | Довжина payload у байтах (unsigned short)    |
| payload  | var  | Корисні дані (0..MAX_PAYLOAD_SIZE байт)      |
| crc32    |  4   | CRC32 від (усі попередні поля) (unsigned int)|
+----------+------+---------------------------------------------+

Заголовок (без payload і crc) = 18 байт. Тотальний overhead = 22 байти.

Припущення щодо часу (див. п.4 ТЗ "Час"):
- `timestamp` — це wall-clock час відправника (time.time() * 1000, мс),
  бо це поле передається МІЖ процесами/машинами і monotonic-час
  одного процесу не має сенсу для іншого.
- Для локальних вимірювань інтервалів (таймаути retry, heartbeat-інтервали,
  online/offline на шлюзі) використовується time.monotonic() —
  це internal, у сам пакет не потрапляє.
- Синхронізація годинників між вузлом і шлюзом НЕ гарантується
  (демо на одній машині/локальній мережі, NTP-дрейф ігнорується).
  timestamp використовується для latency-оцінок лише орієнтовно.
"""
from __future__ import annotations

import struct
import time
from dataclasses import dataclass

from protocol.constants import MAX_PAYLOAD_SIZE, MessageType, PROTOCOL_VERSION

# !BBHIQH -> network byte order, unsigned char, unsigned char, unsigned short,
# unsigned int, unsigned long long, unsigned short
_HEADER_FORMAT = "!BBHIQH"
_HEADER_SIZE = struct.calcsize(_HEADER_FORMAT)  # 18 bytes
_CRC_FORMAT = "!I"
_CRC_SIZE = struct.calcsize(_CRC_FORMAT)  # 4 bytes


class PacketError(Exception):
    """Базова помилка кодека пакета."""


class CorruptPacketError(PacketError):
    """Пакет пошкоджений: не збігається CRC, або порушена структура."""


class PacketTooLargeError(PacketError):
    """payload перевищує MAX_PAYLOAD_SIZE."""


@dataclass
class Packet:
    msg_type: MessageType
    node_id: int
    sequence: int
    payload: bytes = b""
    timestamp_ms: int | None = None  # якщо None -> заповнюється в to_bytes()
    version: int = PROTOCOL_VERSION

    def to_bytes(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD_SIZE:
            raise PacketTooLargeError(
                f"payload {len(self.payload)}B > MAX_PAYLOAD_SIZE {MAX_PAYLOAD_SIZE}B"
            )
        ts = self.timestamp_ms if self.timestamp_ms is not None else int(time.time() * 1000)
        header = struct.pack(
            _HEADER_FORMAT,
            self.version,
            int(self.msg_type),
            self.node_id,
            self.sequence,
            ts,
            len(self.payload),
        )
        body = header + self.payload
        crc = _crc32(body)
        return body + struct.pack(_CRC_FORMAT, crc)

    @staticmethod
    def from_bytes(data: bytes) -> "Packet":
        if len(data) < _HEADER_SIZE + _CRC_SIZE:
            raise CorruptPacketError(
                f"пакет занадто короткий: {len(data)}B < мінімум {_HEADER_SIZE + _CRC_SIZE}B"
            )

        body, crc_bytes = data[:-_CRC_SIZE], data[-_CRC_SIZE:]
        (received_crc,) = struct.unpack(_CRC_FORMAT, crc_bytes)
        actual_crc = _crc32(body)
        if actual_crc != received_crc:
            raise CorruptPacketError(
                f"CRC mismatch: у пакеті записано {received_crc:#010x}, "
                f"перерахований з даних crc = {actual_crc:#010x}"
            )

        header_bytes = body[:_HEADER_SIZE]
        version, msg_type_raw, node_id, sequence, ts, pay_len = struct.unpack(
            _HEADER_FORMAT, header_bytes
        )

        payload = body[_HEADER_SIZE:]
        if len(payload) != pay_len:
            raise CorruptPacketError(
                f"payload_length у заголовку ({pay_len}) не збігається "
                f"з фактичним розміром payload ({len(payload)})"
            )

        try:
            msg_type = MessageType(msg_type_raw)
        except ValueError as exc:
            raise CorruptPacketError(f"невідомий msg_type={msg_type_raw}") from exc

        return Packet(
            msg_type=msg_type,
            node_id=node_id,
            sequence=sequence,
            payload=payload,
            timestamp_ms=ts,
            version=version,
        )


def _crc32(data: bytes) -> int:
    import zlib

    return zlib.crc32(data) & 0xFFFFFFFF

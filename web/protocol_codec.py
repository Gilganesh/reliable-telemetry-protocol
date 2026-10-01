"""
protocol_codec.py -- Python-кодек, сумісний зі СПРАВЖНІМ протоколом на C
(protocol.h/protocol.c), який реально ходить по MQTT зараз.

ВАЖЛИВО: тут НЕ big-endian ("!", мережевий порядок байтів). Реальний C-код
свідомо використовує NATIVE little-endian без htonl
(docs/team-blocks/00-overview-shared-contract.md, розділ "Важливо для
Блоку A") -- бо ESP32, ноутбук і Raspberry Pi всі little-endian.

Перевірено (крос-чек, 30.09): пакет, зібраний справжнім protocol_pack()
у C, розпаковується цим кодеком побайтово правильно, і round-trip
(unpack -> pack) дає ідентичні байти. Команда для перевірки лишається
в самому файлі нижче (`python3 protocol_codec.py`).
"""
import struct
import zlib
from dataclasses import dataclass

MAX_PAYLOAD_SIZE = 128
HEADER_FORMAT = "<BBHIQH"  # '<' = little-endian, точно як memcpy у protocol.c
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)  # 18 байт
CRC_FORMAT = "<I"
CRC_SIZE = 4


class MsgType:
    HEARTBEAT = 0
    TELEMETRY = 1
    ALARM = 2
    CONFIG = 3
    ACK = 4


class CorruptPacketError(Exception):
    pass


@dataclass
class Packet:
    version: int
    msg_type: int
    node_id: int
    sequence: int
    timestamp_ms: int
    payload: bytes

    @staticmethod
    def unpack(raw: bytes) -> "Packet":
        if len(raw) < HEADER_SIZE + CRC_SIZE:
            raise CorruptPacketError(f"занадто короткий пакет: {len(raw)}Б")

        body, crc_bytes = raw[:-CRC_SIZE], raw[-CRC_SIZE:]
        (received_crc,) = struct.unpack(CRC_FORMAT, crc_bytes)
        actual_crc = zlib.crc32(body) & 0xFFFFFFFF
        if actual_crc != received_crc:
            raise CorruptPacketError(
                f"CRC не збігається: у пакеті {received_crc:#010x}, "
                f"порахували {actual_crc:#010x}"
            )

        header = body[:HEADER_SIZE]
        version, msg_type, node_id, sequence, ts, pay_len = struct.unpack(HEADER_FORMAT, header)
        payload = body[HEADER_SIZE:]
        if len(payload) != pay_len:
            raise CorruptPacketError("payload_len не збігається з фактичним розміром")

        return Packet(version, msg_type, node_id, sequence, ts, payload)

    def pack(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD_SIZE:
            raise ValueError("payload задовгий")
        header = struct.pack(
            HEADER_FORMAT,
            self.version, self.msg_type, self.node_id,
            self.sequence, self.timestamp_ms, len(self.payload),
        )
        body = header + self.payload
        crc = zlib.crc32(body) & 0xFFFFFFFF
        return body + struct.pack(CRC_FORMAT, crc)


if __name__ == "__main__":
    # Крос-перевірка проти реального пакета, зібраного справжнім
    # protocol_pack() у C (version=1, TELEMETRY, node_id=7,
    # sequence=0x12345678, timestamp=1234567890123, payload з t=21.4/h=45.2).
    hex_from_c = ("0101070078563412cb04fb711f01000013007b2274223a32312e342c2268223a34352e327de8f51dcb")
    raw = bytes.fromhex(hex_from_c)
    pkt = Packet.unpack(raw)
    assert pkt.version == 1
    assert pkt.msg_type == MsgType.TELEMETRY
    assert pkt.node_id == 7
    assert pkt.sequence == 305419896
    assert pkt.timestamp_ms == 1234567890123
    assert pkt.payload == b'{"t":21.4,"h":45.2}'
    assert pkt.pack() == raw, "round-trip не збігається!"
    print("УСІ ПЕРЕВІРКИ ПРОЙШЛИ -- Python-кодек побайтово сумісний зі справжнім C.")

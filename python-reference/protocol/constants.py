"""
Протокольні константи.

Рішення (за замовчуванням команди, п.9 project-context.md, ще НЕ підтверджено ментором):
- Критичні повідомлення = ALARM + CONFIG. TELEMETRY і HEARTBEAT — best-effort (без ACK).
- version — версія протоколу, поки 1.
"""
from enum import IntEnum


PROTOCOL_VERSION = 1

# Максимальний розмір payload (байти). Обраний так, щоб повний пакет
# (header 18B + payload + crc 4B) не перевищував ~1024B і не фрагментувався
# на типовому Ethernet MTU (1500B).
MAX_PAYLOAD_SIZE = 1000


class MessageType(IntEnum):
    HEARTBEAT = 0   # best-effort, немає ACK
    TELEMETRY = 1   # best-effort, немає ACK
    ALARM = 2       # критичне, потребує ACK/retry
    CONFIG = 3      # критичне, потребує ACK/retry (команда gateway -> node)
    ACK = 4         # підтвердження критичного повідомлення


# Критичні типи — для них потрібен ACK + retry (дедуплікація бізнес-події
# за (node_id, sequence) на прийомній стороні).
CRITICAL_TYPES = frozenset({MessageType.ALARM, MessageType.CONFIG})

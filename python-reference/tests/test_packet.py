"""Unit-тести кодека пакета: round-trip, детект пошкодження, edge cases.

Стандартний unittest (без зовнішніх залежностей) — pip зараз не має
доступу до PyPI в цьому середовищі, тож pytest недоступний.
Запуск: python3 -m unittest tests.test_packet -v
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from protocol.constants import MessageType
from protocol.packet import CorruptPacketError, Packet, PacketTooLargeError, _crc32, _CRC_SIZE


class PacketCodecTests(unittest.TestCase):
    def test_roundtrip_telemetry(self):
        p = Packet(msg_type=MessageType.TELEMETRY, node_id=3, sequence=42, payload=b'{"t":21.5}')
        data = p.to_bytes()
        p2 = Packet.from_bytes(data)
        self.assertEqual(p2.msg_type, MessageType.TELEMETRY)
        self.assertEqual(p2.node_id, 3)
        self.assertEqual(p2.sequence, 42)
        self.assertEqual(p2.payload, b'{"t":21.5}')
        self.assertIsNotNone(p2.timestamp_ms)

    def test_roundtrip_empty_payload_heartbeat(self):
        p = Packet(msg_type=MessageType.HEARTBEAT, node_id=1, sequence=0, payload=b"")
        data = p.to_bytes()
        p2 = Packet.from_bytes(data)
        self.assertEqual(p2.payload, b"")
        self.assertEqual(p2.msg_type, MessageType.HEARTBEAT)

    def test_explicit_timestamp_preserved(self):
        p = Packet(msg_type=MessageType.ALARM, node_id=5, sequence=7, timestamp_ms=1234567890)
        data = p.to_bytes()
        p2 = Packet.from_bytes(data)
        self.assertEqual(p2.timestamp_ms, 1234567890)

    def test_corrupted_single_byte_detected(self):
        p = Packet(msg_type=MessageType.CONFIG, node_id=2, sequence=9, payload=b"config-data")
        data = bytearray(p.to_bytes())
        data[10] ^= 0xFF  # б'ємо один байт у середині
        with self.assertRaises(CorruptPacketError):
            Packet.from_bytes(bytes(data))

    def test_truncated_packet_rejected(self):
        p = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=1, payload=b"x" * 20)
        data = p.to_bytes()
        with self.assertRaises(CorruptPacketError):
            Packet.from_bytes(data[:10])  # обірваний пакет

    def test_too_short_for_header_rejected(self):
        with self.assertRaises(CorruptPacketError):
            Packet.from_bytes(b"\x00\x01")

    def test_random_garbage_rejected(self):
        garbage = bytes(range(30))
        with self.assertRaises(CorruptPacketError):
            Packet.from_bytes(garbage)

    def test_payload_too_large_rejected(self):
        p = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=1, payload=b"x" * 2000)
        with self.assertRaises(PacketTooLargeError):
            p.to_bytes()

    def test_unknown_msg_type_rejected(self):
        p = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=1, payload=b"")
        data = bytearray(p.to_bytes())
        data[1] = 99  # невідомий msg_type
        # перерахувати CRC під нове значення msg_type, щоб CRC пройшов
        # і спрацювала саме перевірка msg_type, а не CRC
        body = bytes(data[:-_CRC_SIZE])
        fixed = body + struct.pack("!I", _crc32(body))
        with self.assertRaises(CorruptPacketError):
            Packet.from_bytes(fixed)

    def test_sequences_are_independent_objects(self):
        # sanity: дві різні послідовності пакетів не плутають CRC одна одної
        p1 = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=1, payload=b"a")
        p2 = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=2, payload=b"b")
        self.assertNotEqual(p1.to_bytes(), p2.to_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)

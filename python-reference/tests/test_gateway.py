"""
Тест критерію приймання #5: пошкоджений пакет відхиляється й логується,
без аварійного завершення роботи gateway. Плюс базова перевірка gap/duplicate.
"""
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from gateway.gateway import Gateway
from protocol.constants import MessageType
from protocol.packet import Packet


class GatewayResilienceTests(unittest.TestCase):
    def setUp(self):
        self.gw = Gateway(listen_addr=("127.0.0.1", 0), log_path="logs/test_gateway.jsonl")

    def tearDown(self):
        self.gw._transport.close()
        self.gw._log_fh.close()

    def test_corrupt_packet_does_not_raise(self):
        garbage = b"\x00\xff\x11\x22\x33garbage-not-a-real-packet"
        try:
            self.gw._handle_datagram(garbage, ("127.0.0.1", 12345))
        except Exception as exc:  # noqa: BLE001
            self.fail(f"биті дані повалили gateway: {exc!r}")
        self.assertEqual(self.gw._corrupt_count, 1)
        self.assertEqual(len(self.gw._nodes), 0)  # жодного вузла не створено з мотлоху

    def test_valid_packet_after_corrupt_still_processed(self):
        # спочатку сміття, потім нормальний пакет — gateway має продовжити роботу
        self.gw._handle_datagram(b"garbage", ("127.0.0.1", 1))
        p = Packet(msg_type=MessageType.TELEMETRY, node_id=7, sequence=0, payload=b"{}")
        self.gw._handle_datagram(p.to_bytes(), ("127.0.0.1", 2))
        self.assertEqual(self.gw._corrupt_count, 1)
        self.assertIn(7, self.gw._nodes)
        self.assertEqual(self.gw._nodes[7].received_count, 1)

    def test_gap_detection(self):
        for seq in (0, 1, 2, 5):  # пропущено 3, 4
            p = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=seq, payload=b"")
            self.gw._handle_datagram(p.to_bytes(), ("127.0.0.1", 1))
        node = self.gw._nodes[1]
        self.assertEqual(node.received_count, 4)
        self.assertEqual(node.lost_count, 2)

    def test_duplicate_detection(self):
        for seq in (0, 1, 1, 2):  # 1 повторюється
            p = Packet(msg_type=MessageType.TELEMETRY, node_id=1, sequence=seq, payload=b"")
            self.gw._handle_datagram(p.to_bytes(), ("127.0.0.1", 1))
        node = self.gw._nodes[1]
        self.assertEqual(node.received_count, 3)
        self.assertEqual(node.duplicate_count, 1)

    def test_multiple_nodes_tracked_separately(self):
        for node_id in (1, 2, 3):
            for seq in range(3):
                p = Packet(msg_type=MessageType.TELEMETRY, node_id=node_id, sequence=seq, payload=b"")
                self.gw._handle_datagram(p.to_bytes(), ("127.0.0.1", node_id))
        self.assertEqual(len(self.gw._nodes), 3)
        for node_id in (1, 2, 3):
            self.assertEqual(self.gw._nodes[node_id].received_count, 3)
            self.assertEqual(self.gw._nodes[node_id].lost_count, 0)

    def test_offline_detection(self):
        p = Packet(msg_type=MessageType.HEARTBEAT, node_id=9, sequence=0, payload=b"")
        self.gw._handle_datagram(p.to_bytes(), ("127.0.0.1", 9))
        node = self.gw._nodes[9]
        self.assertTrue(node.is_online(timeout=10.0))
        # штучно "заморожуємо" last_seen у минуле
        node.last_seen_monotonic = time.monotonic() - 100
        self.assertFalse(node.is_online(timeout=10.0))


if __name__ == "__main__":
    unittest.main(verbosity=2)

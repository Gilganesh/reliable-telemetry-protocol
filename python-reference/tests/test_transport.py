"""Sanity-тест UDP-транспорту: send/recv на loopback."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from transport.udp_transport import UDPTransport


class UDPTransportTests(unittest.TestCase):
    def test_send_recv_roundtrip(self):
        server = UDPTransport(bind_addr=("127.0.0.1", 0))
        server_addr = server.local_addr()
        client = UDPTransport()

        client.send(b"hello", server_addr)
        result = server.recv(timeout=2.0)

        self.assertIsNotNone(result)
        data, addr = result
        self.assertEqual(data, b"hello")

        server.close()
        client.close()

    def test_recv_timeout_returns_none(self):
        server = UDPTransport(bind_addr=("127.0.0.1", 0))
        result = server.recv(timeout=0.2)
        self.assertIsNone(result)
        server.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)

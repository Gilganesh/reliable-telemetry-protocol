"""
Sensor Node — Day 1: heartbeat + telemetry з sequence number.

Навмисно НЕ включено (буде на День 2, за project-context.md):
- ACK/retry для критичних повідомлень (ALARM/CONFIG)
- буфер + store-and-forward при розриві зв'язку
- прийом CONFIG від gateway

Запуск (окремий процес на кожен вузол):
    python3 -m node.sensor_node --node-id 1 --gateway-port 9999
"""
from __future__ import annotations

import argparse
import json
import logging
import random
import time

from protocol.constants import MessageType
from protocol.packet import Packet
from transport.base import Address
from transport.udp_transport import UDPTransport

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)


class SensorNode:
    def __init__(
        self,
        node_id: int,
        gateway_addr: Address,
        heartbeat_interval: float = 5.0,
        telemetry_interval: float = 2.0,
    ):
        self.node_id = node_id
        self.gateway_addr = gateway_addr
        self.heartbeat_interval = heartbeat_interval
        self.telemetry_interval = telemetry_interval

        self._transport = UDPTransport()
        self._sequence = 0  # глобальний лічильник для ЦЬОГО вузла (крос-типовий)
        self._log = logging.getLogger(f"node-{node_id}")

        # "стан датчика" для реалістичних значень, що плавно змінюються
        self._temperature = 20.0 + random.uniform(-2, 2)
        self._humidity = 45.0 + random.uniform(-5, 5)

    def _next_sequence(self) -> int:
        seq = self._sequence
        self._sequence += 1
        return seq

    def _send(self, msg_type: MessageType, payload: bytes = b"") -> int:
        seq = self._next_sequence()
        packet = Packet(
            msg_type=msg_type,
            node_id=self.node_id,
            sequence=seq,
            payload=payload,
        )
        self._transport.send(packet.to_bytes(), self.gateway_addr)
        return seq

    def _read_sensors(self) -> dict:
        # невеликий випадковий дрейф, щоб дані виглядали "живими"
        self._temperature += random.uniform(-0.3, 0.3)
        self._humidity += random.uniform(-1.0, 1.0)
        return {
            "temp_c": round(self._temperature, 2),
            "humidity_pct": round(self._humidity, 2),
        }

    def send_heartbeat(self) -> None:
        seq = self._send(MessageType.HEARTBEAT)
        self._log.info("HEARTBEAT seq=%d", seq)

    def send_telemetry(self) -> None:
        reading = self._read_sensors()
        payload = json.dumps(reading).encode("utf-8")
        seq = self._send(MessageType.TELEMETRY, payload)
        self._log.info("TELEMETRY seq=%d payload=%s", seq, reading)

    def run(self) -> None:
        self._log.info(
            "старт: gateway=%s, heartbeat=%.1fs, telemetry=%.1fs",
            self.gateway_addr,
            self.heartbeat_interval,
            self.telemetry_interval,
        )
        next_heartbeat = time.monotonic()
        next_telemetry = time.monotonic()
        try:
            while True:
                now = time.monotonic()
                if now >= next_heartbeat:
                    self.send_heartbeat()
                    next_heartbeat = now + self.heartbeat_interval
                if now >= next_telemetry:
                    self.send_telemetry()
                    next_telemetry = now + self.telemetry_interval
                time.sleep(0.1)
        except KeyboardInterrupt:
            self._log.info("зупинка (Ctrl+C)")
        finally:
            self._transport.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Sensor Node (Day 1)")
    parser.add_argument("--node-id", type=int, required=True)
    parser.add_argument("--gateway-host", default="127.0.0.1")
    parser.add_argument("--gateway-port", type=int, required=True)
    parser.add_argument("--heartbeat-interval", type=float, default=5.0)
    parser.add_argument("--telemetry-interval", type=float, default=2.0)
    args = parser.parse_args()

    node = SensorNode(
        node_id=args.node_id,
        gateway_addr=(args.gateway_host, args.gateway_port),
        heartbeat_interval=args.heartbeat_interval,
        telemetry_interval=args.telemetry_interval,
    )
    node.run()


if __name__ == "__main__":
    main()

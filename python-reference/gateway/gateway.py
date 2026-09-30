"""
Gateway — Day 1: прийом від N вузлів окремо, захист від битих пакетів,
підрахунок пропусків/дублікатів, простий термінальний dashboard + JSON-логи.

Навмисно НЕ включено (буде на День 2):
- відправка ACK на критичні повідомлення
- точна дедуплікація бізнес-подій за (node_id, sequence)
- online/offline -> триггер прийому store-and-forward backlog

Запуск:
    python3 -m gateway.gateway --port 9999
"""
from __future__ import annotations

import argparse
import json
import logging
import time
from pathlib import Path
from typing import Dict

from gateway.node_state import NodeState
from protocol.packet import Packet, PacketError
from transport.udp_transport import UDPTransport

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)


class Gateway:
    def __init__(
        self,
        listen_addr,
        offline_timeout: float = 15.0,
        dashboard_interval: float = 2.0,
        log_path: str = "logs/gateway.jsonl",
    ):
        self.offline_timeout = offline_timeout
        self.dashboard_interval = dashboard_interval

        self._transport = UDPTransport(bind_addr=listen_addr)
        self._nodes: Dict[int, NodeState] = {}
        self._corrupt_count = 0
        self._known_online: Dict[int, bool] = {}  # для детекту зміни статусу

        self._log = logging.getLogger("gateway")
        log_file = Path(log_path)
        log_file.parent.mkdir(parents=True, exist_ok=True)
        self._log_fh = log_file.open("a", encoding="utf-8")

    # ---- журналювання ----------------------------------------------------

    def _log_event(self, event: str, **fields) -> None:
        record = {"ts": time.time(), "event": event, **fields}
        self._log_fh.write(json.dumps(record, ensure_ascii=False) + "\n")
        self._log_fh.flush()

    # ---- обробка одного пакета --------------------------------------------

    def _handle_datagram(self, data: bytes, addr) -> None:
        try:
            packet = Packet.from_bytes(data)
        except PacketError as exc:
            self._corrupt_count += 1
            self._log.warning("ВІДХИЛЕНО биті пакет від %s: %s", addr, exc)
            self._log_event("packet_corrupt", addr=str(addr), error=str(exc))
            return  # критерій приймання #5: не падаємо, лог і йдемо далі

        node = self._nodes.setdefault(packet.node_id, NodeState(node_id=packet.node_id))
        category = node.record_packet(packet.sequence, addr)

        self._log.info(
            "node=%d type=%s seq=%d [%s] (recv=%d lost=%d dup=%d)",
            packet.node_id,
            packet.msg_type.name,
            packet.sequence,
            category,
            node.received_count,
            node.lost_count,
            node.duplicate_count,
        )
        self._log_event(
            "packet_received",
            node_id=packet.node_id,
            msg_type=packet.msg_type.name,
            sequence=packet.sequence,
            category=category,
            payload_size=len(packet.payload),
        )

        if category == "gap":
            self._log_event(
                "packet_gap_detected",
                node_id=packet.node_id,
                up_to_sequence=packet.sequence,
                lost_total=node.lost_count,
            )

    # ---- dashboard ----------------------------------------------------

    def _check_status_changes(self) -> None:
        for node_id, node in self._nodes.items():
            online = node.is_online(self.offline_timeout)
            prev = self._known_online.get(node_id)
            if prev is not None and prev != online:
                status = "online" if online else "offline"
                self._log.warning("вузол %d змінив статус -> %s", node_id, status.upper())
                self._log_event("node_status_change", node_id=node_id, status=status)
            self._known_online[node_id] = online

    def _print_dashboard(self) -> None:
        self._check_status_changes()
        lines = [
            "",
            "=" * 78,
            f"  DASHBOARD  |  вузлів: {len(self._nodes)}  |  битих пакетів: {self._corrupt_count}",
            "-" * 78,
            f"{'node_id':>7} | {'status':>7} | {'last_seq':>8} | {'recv':>6} | "
            f"{'lost':>5} | {'dup':>4} | {'loss%':>6} | {'seen_ago':>8}",
        ]
        for node_id in sorted(self._nodes):
            node = self._nodes[node_id]
            online = node.is_online(self.offline_timeout)
            lines.append(
                f"{node_id:>7} | {'ONLINE' if online else 'OFFLINE':>7} | "
                f"{node.max_seq_seen:>8} | {node.received_count:>6} | "
                f"{node.lost_count:>5} | {node.duplicate_count:>4} | "
                f"{node.loss_rate() * 100:>5.1f}% | {node.seconds_since_seen():>7.1f}s"
            )
        lines.append("=" * 78)
        print("\n".join(lines))

    # ---- головний цикл --------------------------------------------------

    def run(self) -> None:
        self._log.info("gateway слухає на %s", self._transport.local_addr())
        next_dashboard = time.monotonic()
        try:
            while True:
                result = self._transport.recv(timeout=0.5)
                if result is not None:
                    data, addr = result
                    try:
                        self._handle_datagram(data, addr)
                    except Exception:  # noqa: BLE001 — захист від будь-якого падіння
                        self._corrupt_count += 1
                        self._log.exception("неочікувана помилка обробки пакета від %s", addr)

                now = time.monotonic()
                if now >= next_dashboard:
                    self._print_dashboard()
                    next_dashboard = now + self.dashboard_interval
        except KeyboardInterrupt:
            self._log.info("зупинка (Ctrl+C)")
        finally:
            self._transport.close()
            self._log_fh.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="Gateway (Day 1)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--offline-timeout", type=float, default=15.0)
    parser.add_argument("--dashboard-interval", type=float, default=2.0)
    parser.add_argument("--log-path", default="logs/gateway.jsonl")
    args = parser.parse_args()

    gw = Gateway(
        listen_addr=(args.host, args.port),
        offline_timeout=args.offline_timeout,
        dashboard_interval=args.dashboard_interval,
        log_path=args.log_path,
    )
    gw.run()


if __name__ == "__main__":
    main()

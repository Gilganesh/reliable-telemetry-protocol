"""Стан одного вузла з точки зору gateway."""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Optional

from transport.base import Address


@dataclass
class NodeState:
    node_id: int
    addr: Optional[Address] = None

    max_seq_seen: int = -1  # -1 = ще жодного пакета не отримано
    received_count: int = 0
    lost_count: int = 0       # сумарна кількість пропущених (за gap-аналізом)
    duplicate_count: int = 0  # seq <= max_seq_seen (дублікат або сильно out-of-order)

    last_seen_monotonic: float = field(default_factory=time.monotonic)
    first_seen_monotonic: float = field(default_factory=time.monotonic)

    def record_packet(self, seq: int, addr: Address) -> str:
        """Оновити стан на основі отриманого sequence number.
        Повертає категорію пакета: "new" | "gap" | "duplicate".

        Обмеження Day 1 (документується як відомий технічний борг):
        дублікат і "прийшов із запізненням" (out-of-order) наразі
        не розрізняються — обидва потрапляють у duplicate_count.
        Точна дедуплікація за (node_id, sequence) для бізнес-подій
        буде додана на День 2 разом з ACK/retry.
        """
        self.addr = addr
        self.last_seen_monotonic = time.monotonic()

        if self.max_seq_seen == -1:
            # перший пакет від цього вузла
            self.max_seq_seen = seq
            self.received_count += 1
            return "new"

        if seq > self.max_seq_seen:
            gap = seq - self.max_seq_seen - 1
            self.lost_count += gap
            self.max_seq_seen = seq
            self.received_count += 1
            return "gap" if gap > 0 else "new"

        self.duplicate_count += 1
        return "duplicate"

    def is_online(self, timeout: float) -> bool:
        return (time.monotonic() - self.last_seen_monotonic) <= timeout

    def seconds_since_seen(self) -> float:
        return time.monotonic() - self.last_seen_monotonic

    def loss_rate(self) -> float:
        total_expected = self.received_count + self.lost_count
        if total_expected == 0:
            return 0.0
        return self.lost_count / total_expected

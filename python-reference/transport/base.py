"""
Абстрактний транспортний рівень.

Мета: логіка протоколу (node/gateway) працює лише з Transport,
не знаючи, що під капотом — UDP, TCP, UART чи імітований канал.
Заміна транспорту = нова реалізація цього інтерфейсу, без змін
у Sensor Node / Gateway.
"""
from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Optional, Tuple

Address = Tuple[str, int]  # (host, port) — для UDP/TCP. Для UART це буде інший тип адреси.


class Transport(ABC):
    """Мінімальний контракт транспорту: send/recv по датаграмах."""

    @abstractmethod
    def send(self, data: bytes, addr: Optional[Address] = None) -> None:
        """Надіслати сирі байти. addr обов'язковий для клієнта (node),
        не потрібен для сервера, що вже "прив'язаний" до конкретного peer
        (не наш випадок — гейтвей приймає від багатьох, тож теж передає addr)."""

    @abstractmethod
    def recv(self, timeout: Optional[float] = None) -> Optional[Tuple[bytes, Address]]:
        """Отримати (дані, адреса_відправника) або None, якщо вийшов timeout.
        Не піднімає виняток при timeout — це нормальний робочий стан,
        а не помилка каналу."""

    @abstractmethod
    def close(self) -> None:
        """Звільнити ресурси (закрити socket/файл/порт)."""

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()

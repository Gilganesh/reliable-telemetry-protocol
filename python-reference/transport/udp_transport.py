"""UDP-реалізація Transport."""
from __future__ import annotations

import socket
from typing import Optional, Tuple

from transport.base import Address, Transport


class UDPTransport(Transport):
    def __init__(self, bind_addr: Optional[Address] = None):
        """
        bind_addr: якщо задано (host, port) — socket прив'язується до цієї
        адреси (типово для gateway, який слухає вхідні). Якщо None —
        socket отримає довільний вільний порт від ОС (типово для node-клієнта).
        """
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        if bind_addr is not None:
            self._sock.bind(bind_addr)

    def send(self, data: bytes, addr: Optional[Address] = None) -> None:
        if addr is None:
            raise ValueError("UDPTransport.send: потрібна адреса призначення (addr)")
        self._sock.sendto(data, addr)

    def recv(self, timeout: Optional[float] = None) -> Optional[Tuple[bytes, Address]]:
        self._sock.settimeout(timeout)
        try:
            data, addr = self._sock.recvfrom(65535)
            return data, addr
        except socket.timeout:
            return None

    def local_addr(self) -> Address:
        return self._sock.getsockname()

    def close(self) -> None:
        self._sock.close()

import struct
import zlib
from dataclasses import dataclass

PROTOCOL_VERSION = 1
MAX_PAYLOAD_SIZE = 128
HEADER_FORMAT = "<BBHIQH"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
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
            raise CorruptPacketError(f"packet too short: {len(raw)} bytes")

        body, crc_bytes = raw[:-CRC_SIZE], raw[-CRC_SIZE:]
        (received_crc,) = struct.unpack(CRC_FORMAT, crc_bytes)
        actual_crc = zlib.crc32(body) & 0xFFFFFFFF
        if actual_crc != received_crc:
            raise CorruptPacketError(
                f"CRC mismatch: packet has {received_crc:#010x}, "
                f"computed {actual_crc:#010x}"
            )

        header = body[:HEADER_SIZE]
        version, msg_type, node_id, sequence, ts, pay_len = struct.unpack(HEADER_FORMAT, header)
        if version != PROTOCOL_VERSION:
            raise CorruptPacketError(f"unsupported protocol version {version}")
        payload = body[HEADER_SIZE:]
        if len(payload) != pay_len:
            raise CorruptPacketError("payload_len does not match the actual payload size")

        return Packet(version, msg_type, node_id, sequence, ts, payload)

    def pack(self) -> bytes:
        if len(self.payload) > MAX_PAYLOAD_SIZE:
            raise ValueError("payload too long")
        header = struct.pack(
            HEADER_FORMAT,
            self.version, self.msg_type, self.node_id,
            self.sequence, self.timestamp_ms, len(self.payload),
        )
        body = header + self.payload
        crc = zlib.crc32(body) & 0xFFFFFFFF
        return body + struct.pack(CRC_FORMAT, crc)

if __name__ == "__main__":
    hex_from_c = ("0101070078563412cb04fb711f01000013007b2274223a32312e342c2268223a34352e327de8f51dcb")
    raw = bytes.fromhex(hex_from_c)
    pkt = Packet.unpack(raw)
    assert pkt.version == 1
    assert pkt.msg_type == MsgType.TELEMETRY
    assert pkt.node_id == 7
    assert pkt.sequence == 305419896
    assert pkt.timestamp_ms == 1234567890123
    assert pkt.payload == b'{"t":21.4,"h":45.2}'
    assert pkt.pack() == raw, "round-trip mismatch"
    foreign = Packet(PROTOCOL_VERSION + 6, MsgType.TELEMETRY, 7, 1, 0, b"{}").pack()
    try:
        Packet.unpack(foreign)
    except CorruptPacketError:
        pass
    else:
        raise AssertionError("a frame with another protocol version was accepted")
    print("OK: Python codec is byte-compatible with the C implementation.")

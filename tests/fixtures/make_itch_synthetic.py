"""Writes tests/fixtures/itch_synthetic.bin: a tiny, invented file in the real ITCH 5.0 format.

Nothing here comes from market data. Run it from the repository root after changing the flow.
"""

import struct

OPEN_NS = 34_200_000_000_000


def header(kind: bytes, locate: int, ts: int) -> bytes:
    return kind + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")


def stock(name: str) -> bytes:
    return name.ljust(8).encode()


def directory(locate: int, name: str) -> bytes:
    return header(b"R", locate, 1000) + stock(name) + bytes(20)


def add(locate: int, name: str, ts: int, ref: int, side: bytes, shares: int, price: int) -> bytes:
    body = struct.pack(">Q", ref) + side + struct.pack(">I", shares)
    return header(b"A", locate, ts) + body + stock(name) + struct.pack(">I", price)


def execute(locate: int, ts: int, ref: int, shares: int) -> bytes:
    return header(b"E", locate, ts) + struct.pack(">QIQ", ref, shares, 1)


def cancel(locate: int, ts: int, ref: int, shares: int) -> bytes:
    return header(b"X", locate, ts) + struct.pack(">QI", ref, shares)


def delete(locate: int, ts: int, ref: int) -> bytes:
    return header(b"D", locate, ts) + struct.pack(">Q", ref)


def replace(locate: int, ts: int, ref: int, new_ref: int, shares: int, price: int) -> bytes:
    return header(b"U", locate, ts) + struct.pack(">QQII", ref, new_ref, shares, price)


t = OPEN_NS
messages = [
    directory(1, "ZZZT"),
    directory(2, "ZYYT"),
    add(1, "ZZZT", t - 5, 1, b"B", 100, 1000000),  # before the open: skipped
    add(1, "ZZZT", t + 1, 2, b"B", 100, 1000000),
    add(1, "ZZZT", t + 2, 3, b"B", 200, 1000000),
    add(1, "ZZZT", t + 3, 4, b"S", 100, 1010000),
    add(1, "ZZZT", t + 4, 5, b"S", 50, 1010050),  # not a whole cent: dropped
    execute(1, t + 5, 2, 40),
    cancel(1, t + 6, 3, 50),
    replace(1, t + 7, 4, 6, 80, 1020000),
    add(1, "ZZZT", t + 8, 7, b"B", 10, 1030000),  # crosses order 6
    delete(1, t + 9, 3),
    delete(1, t + 10, 1),  # unknown: its add was before the open
    add(2, "ZYYT", t + 1, 100, b"B", 10, 500000),
    delete(2, t + 2, 100),
]

with open("tests/fixtures/itch_synthetic.bin", "wb") as out:
    for message in messages:
        out.write(struct.pack(">H", len(message)) + message)

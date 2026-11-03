"""Canonical book_l3 Parquet to .mlog plus sidecar (FR-21).

ReplayBuilder mirrors src/workload/replay.cpp line for line, because D-04 requires both routes to
give identical .mlog semantics. tests/test_book_l3.py pins the mapping, and the local-data test
compares this converter's records with llme-itch-decode's.
"""

from __future__ import annotations

import hashlib
import json
import struct
import subprocess
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Annotated, Any, BinaryIO
from zoneinfo import ZoneInfo

import polars as pl
import typer

app = typer.Typer(add_completion=False)

SYNTHETIC_ID_BIT = 1 << 63
RAW_PER_TICK = 100
TICK_SIZE_E9 = 10_000_000
MAX_QTY = 2**32 - 1
SESSION_START_NS = 34_200_000_000_000
SESSION_END_NS = 57_600_000_000_000

MSG_NEW = 1
MSG_CANCEL = 2
MSG_MODIFY = 3
TIF_GTC = 0
TIF_IOC = 1
SOURCE_BOOK_L3 = 1

HEADER = struct.Struct("<8sHHIqQQB23s")
IN_MSG = struct.Struct("<BBBBIQqII")


@dataclass
class Tracked:
    side: int
    price: int
    leaves: int


@dataclass
class ConversionStats:
    events: int = 0
    adds: int = 0
    execs: int = 0
    partial_cancels: int = 0
    deletes: int = 0
    replaces: int = 0
    out_new: int = 0
    out_ioc: int = 0
    out_modify: int = 0
    out_cancel: int = 0
    dropped_price: int = 0
    dropped_qty: int = 0
    dropped_unknown_id: int = 0
    skipped_session: int = 0
    skipped_cross: int = 0
    max_live_orders: int = 0
    min_ticks: int | None = None
    max_ticks: int | None = None


@dataclass
class ReplayBuilder:
    """Turns book_l3 events into packed InMsg records."""

    records: list[bytes] = field(default_factory=list)
    stats: ConversionStats = field(default_factory=ConversionStats)
    orders: dict[int, Tracked] = field(default_factory=dict)

    def _find(self, order_id: int) -> Tracked | None:
        order = self.orders.get(order_id)
        if order is None:
            self.stats.dropped_unknown_id += 1
        return order

    def add(self, order_id: int, side: int, raw_price: int, qty: int) -> None:
        s = self.stats
        s.events += 1
        s.adds += 1
        if qty <= 0 or qty > MAX_QTY:
            s.dropped_qty += 1
            return
        if order_id & SYNTHETIC_ID_BIT:
            s.dropped_unknown_id += 1
            return
        if raw_price % RAW_PER_TICK != 0:
            s.dropped_price += 1
            return
        price = raw_price // RAW_PER_TICK
        self.orders[order_id] = Tracked(side, price, qty)
        s.max_live_orders = max(s.max_live_orders, len(self.orders))
        s.min_ticks = price if s.min_ticks is None else min(s.min_ticks, price)
        s.max_ticks = price if s.max_ticks is None else max(s.max_ticks, price)
        s.out_new += 1
        self.records.append(IN_MSG.pack(MSG_NEW, side, 0, TIF_GTC, qty, order_id, price, 0, 0))

    def _shrink(self, order_id: int, order: Tracked, removed: int) -> None:
        order.leaves -= removed
        if order.leaves == 0:
            del self.orders[order_id]

    def execute(self, order_id: int, qty: int) -> None:
        s = self.stats
        s.events += 1
        s.execs += 1
        if qty <= 0 or qty > MAX_QTY:
            s.dropped_qty += 1
            return
        order = self._find(order_id)
        if order is None:
            return
        filled = min(qty, order.leaves)
        s.out_ioc += 1
        self.records.append(
            IN_MSG.pack(
                MSG_NEW,
                1 - order.side,
                0,
                TIF_IOC,
                filled,
                order_id | SYNTHETIC_ID_BIT,
                order.price,
                0,
                0,
            )
        )
        self._shrink(order_id, order, filled)

    def partial_cancel(self, order_id: int, qty: int) -> None:
        s = self.stats
        s.events += 1
        s.partial_cancels += 1
        if qty <= 0 or qty > MAX_QTY:
            s.dropped_qty += 1
            return
        self._reduce(order_id, qty)

    def _reduce(self, order_id: int, qty: int) -> None:
        order = self._find(order_id)
        if order is None:
            return
        removed = min(qty, order.leaves)
        self.stats.out_modify += 1
        self.records.append(
            IN_MSG.pack(MSG_MODIFY, 0, 0, 0, order.leaves - removed, order_id, order.price, 0, 0)
        )
        self._shrink(order_id, order, removed)

    def delete(self, order_id: int) -> None:
        s = self.stats
        s.events += 1
        s.deletes += 1
        if self._find(order_id) is None:
            return
        s.out_cancel += 1
        self.records.append(IN_MSG.pack(MSG_CANCEL, 0, 0, 0, 0, order_id, 0, 0, 0))
        del self.orders[order_id]

    def replace(self, order_id: int, new_order_id: int, raw_price: int, qty: int) -> None:
        s = self.stats
        s.events += 1
        s.replaces += 1
        order = self._find(order_id)
        if order is None:
            return
        side = order.side
        s.out_cancel += 1
        self.records.append(IN_MSG.pack(MSG_CANCEL, 0, 0, 0, 0, order_id, 0, 0, 0))
        del self.orders[order_id]
        self.add(new_order_id, side, raw_price, qty)
        s.events -= 1
        s.adds -= 1

    def cross_exec(self, order_id: int, qty: int) -> None:
        s = self.stats
        s.events += 1
        s.skipped_cross += 1
        if qty <= 0 or qty > MAX_QTY:
            s.dropped_qty += 1
            return
        self._reduce(order_id, qty)


def session_bounds(date: str) -> tuple[int, int]:
    """Epoch nanoseconds of 09:30 and 16:00 New York time on this date (DQ-05)."""
    midnight = datetime.fromisoformat(date).replace(tzinfo=ZoneInfo("America/New_York"))
    epoch_ns = int(midnight.timestamp()) * 1_000_000_000
    return epoch_ns + SESSION_START_NS, epoch_ns + SESSION_END_NS


def wire_side(book_l3_side: int) -> int:
    """book_l3 has 1 for buy and -1 for sell; the wire format has 0 and 1."""
    if book_l3_side == 1:
        return 0
    if book_l3_side == -1:
        return 1
    raise ValueError(f"unknown book_l3 side: {book_l3_side}")


def convert_events(events: pl.DataFrame, date: str) -> ReplayBuilder:
    """Applies the book_l3 to .mlog mapping of spec section 7(b), in `seq` order (DQ-03)."""
    builder = ReplayBuilder()
    start_ns, end_ns = session_bounds(date)
    columns = ["ts_exchange", "event", "order_id", "side", "price_ticks", "qty"]
    columns += ["new_order_id", "printable"]
    for ts, event, order_id, side, price, qty, new_id, printable in (
        events.sort("seq").select(columns).iter_rows()
    ):
        if ts < start_ns or ts >= end_ns:
            builder.stats.skipped_session += 1
        elif event == "add":
            builder.add(order_id, wire_side(side), price, qty)
        elif event == "exec":
            if printable is False:
                builder.cross_exec(order_id, qty)
            else:
                builder.execute(order_id, qty)
        elif event == "cancel":
            builder.partial_cancel(order_id, qty)
        elif event == "delete":
            builder.delete(order_id)
        elif event == "replace":
            builder.replace(order_id, new_id, price, qty)
        else:
            raise ValueError(f"unknown book_l3 event: {event}")
    return builder


def replay_config(stats: ConversionStats) -> dict[str, int]:
    """DQ-07: observed band plus 10%, live orders times 1.5 rounded up to a power of two."""
    low = stats.min_ticks if stats.min_ticks is not None else 0
    high = stats.max_ticks if stats.max_ticks is not None else 0
    margin = (high - low) // 10 + 1
    wanted = stats.max_live_orders * 3 // 2 + 1
    max_orders = 1 << (wanted - 1).bit_length() if wanted > 1 else 1
    return {"price_min": max(low - margin, 0), "price_max": high + margin, "max_orders": max_orders}


def write_log(out: BinaryIO, records: list[bytes], symbol_id: int = 0) -> None:
    magic = b"LLMEMLOG"
    out.write(
        HEADER.pack(
            magic,
            1,
            IN_MSG.size,
            symbol_id,
            TICK_SIZE_E9,
            len(records),
            0,
            SOURCE_BOOK_L3,
            bytes(23),
        )
    )
    out.writelines(records)


def sidecar(
    source_files: list[Path], symbol: str, builder: ReplayBuilder, config: dict[str, int]
) -> dict[str, Any]:
    s = builder.stats
    digest = hashlib.sha256()
    for path in source_files:
        digest.update(path.read_bytes())
    return {
        "source_file": ",".join(path.name for path in source_files),
        "source_sha256": digest.hexdigest(),
        "symbol": symbol,
        "tick_size_e9": TICK_SIZE_E9,
        **config,
        "min_ticks_observed": s.min_ticks,
        "max_ticks_observed": s.max_ticks,
        "max_live_orders": s.max_live_orders,
        "events": {
            "events": s.events,
            "adds": s.adds,
            "execs": s.execs,
            "partial_cancels": s.partial_cancels,
            "deletes": s.deletes,
            "replaces": s.replaces,
        },
        "mapped": {
            "new": s.out_new,
            "ioc": s.out_ioc,
            "modify": s.out_modify,
            "cancel": s.out_cancel,
        },
        "dropped": {
            "price": s.dropped_price,
            "qty": s.dropped_qty,
            "unknown_id": s.dropped_unknown_id,
        },
        "skipped": {"session": s.skipped_session, "hidden": 0, "cross": s.skipped_cross, "halt": 0},
        "exec_contra_encoding": "IOC order_id with the top bit cleared is the expected contra",
    }


DEFAULT_LLME_RUN = "~/build/low-latency-matching-engine/release/src/tools/llme-run"


@app.command()
def main(
    input_dir: Annotated[Path, typer.Option("--in", help="book_l3 root, LLME_BOOK_L3_DIR")],
    symbol: Annotated[str, typer.Option(help="Symbol")],
    date: Annotated[str, typer.Option(help="Date, YYYY-MM-DD")],
    out: Annotated[Path, typer.Option(help="Output .mlog path")],
    llme_run: Annotated[
        str, typer.Option("--llme-run", help="llme-run binary, for the replay figures")
    ] = DEFAULT_LLME_RUN,
) -> None:
    """Convert one symbol-day of book_l3 events to an input log and sidecar."""
    partition = input_dir / "venue=XNAS" / f"symbol={symbol}" / f"date={date}"
    files = sorted(partition.glob("*.parquet"))
    if not files:
        raise typer.BadParameter(f"no parquet files under {partition}")

    builder = convert_events(pl.read_parquet(files), date)
    config = replay_config(builder.stats)
    with out.open("wb") as handle:
        write_log(handle, builder.records)

    meta = sidecar(files, symbol, builder, config)
    meta_path = out.with_suffix(".meta.json")
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")

    report = subprocess.run(
        [str(Path(llme_run).expanduser()), "--in", str(out), "--replay-stats"],
        check=True,
        capture_output=True,
        text=True,
    )
    meta["replay"] = json.loads(report.stdout)
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    typer.echo(f"{len(builder.records)} messages written to {out}")

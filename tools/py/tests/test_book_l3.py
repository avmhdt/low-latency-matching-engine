"""The book_l3 mapping of spec section 7(b), on hand-built events."""

from __future__ import annotations

import io
import struct

import polars as pl
import pytest

from llme_tools.book_l3_to_mlog import (
    HEADER,
    IN_MSG,
    SYNTHETIC_ID_BIT,
    ReplayBuilder,
    convert_events,
    replay_config,
    session_bounds,
    write_log,
)

DATE = "2019-01-30"
OPEN_NS = session_bounds(DATE)[0]


def frame(rows: list[dict[str, object]]) -> pl.DataFrame:
    defaults = {"side": 0, "price_ticks": 0, "qty": 0, "new_order_id": None, "printable": None}
    filled = []
    for seq, row in enumerate(rows):
        filled.append({"seq": seq, "ts_exchange": OPEN_NS + seq, **defaults, **row})
    return pl.DataFrame(filled)


def records(builder: ReplayBuilder) -> list[tuple[int, ...]]:
    return [IN_MSG.unpack(r) for r in builder.records]


def test_record_layouts_match_the_wire_format() -> None:
    assert HEADER.size == 64
    assert IN_MSG.size == 32


def test_session_is_new_york_time() -> None:
    start, end = session_bounds(DATE)
    assert (end - start) // 1_000_000_000 == 6 * 3600 + 1800
    assert start == 1548858600 * 1_000_000_000  # 14:30 UTC in January


def test_add_cancel_delete_map_to_new_modify_cancel() -> None:
    builder = convert_events(
        frame(
            [
                {"event": "add", "order_id": 1, "side": 1, "price_ticks": 1234500, "qty": 100},
                {"event": "cancel", "order_id": 1, "qty": 30},
                {"event": "delete", "order_id": 1},
            ]
        ),
        DATE,
    )
    assert records(builder) == [
        (1, 1, 0, 0, 100, 1, 12345, 0, 0),
        (3, 0, 0, 0, 70, 1, 12345, 0, 0),
        (2, 0, 0, 0, 0, 1, 0, 0, 0),
    ]


def test_exec_becomes_ioc_on_the_opposite_side_with_the_contra_in_the_id() -> None:
    builder = convert_events(
        frame(
            [
                {"event": "add", "order_id": 7, "side": 0, "price_ticks": 1000000, "qty": 10},
                {"event": "exec", "order_id": 7, "qty": 4, "printable": True},
            ]
        ),
        DATE,
    )
    assert records(builder)[1] == (1, 1, 0, 1, 4, 7 | SYNTHETIC_ID_BIT, 10000, 0, 0)
    assert builder.stats.out_ioc == 1


def test_replace_is_cancel_then_new_on_the_same_side() -> None:
    builder = convert_events(
        frame(
            [
                {"event": "add", "order_id": 1, "side": 1, "price_ticks": 1000000, "qty": 10},
                {
                    "event": "replace",
                    "order_id": 1,
                    "new_order_id": 2,
                    "price_ticks": 1010000,
                    "qty": 5,
                },
            ]
        ),
        DATE,
    )
    assert records(builder)[1:] == [
        (2, 0, 0, 0, 0, 1, 0, 0, 0),
        (1, 1, 0, 0, 5, 2, 10100, 0, 0),
    ]
    assert builder.stats.adds == 1
    assert builder.stats.replaces == 1


def test_non_printable_exec_reduces_without_matching() -> None:
    builder = convert_events(
        frame(
            [
                {"event": "add", "order_id": 1, "side": 0, "price_ticks": 1000000, "qty": 10},
                {"event": "exec", "order_id": 1, "qty": 4, "printable": False},
            ]
        ),
        DATE,
    )
    assert records(builder)[1] == (3, 0, 0, 0, 6, 1, 10000, 0, 0)
    assert builder.stats.skipped_cross == 1


def test_inexact_prices_unknown_ids_and_bad_quantities_are_dropped() -> None:
    builder = convert_events(
        frame(
            [
                {"event": "add", "order_id": 1, "side": 0, "price_ticks": 1000050, "qty": 10},
                {"event": "delete", "order_id": 1},
                {"event": "add", "order_id": 2, "side": 0, "price_ticks": 1000000, "qty": 0},
            ]
        ),
        DATE,
    )
    assert builder.records == []
    assert builder.stats.dropped_price == 1
    assert builder.stats.dropped_unknown_id == 1
    assert builder.stats.dropped_qty == 1


def test_events_outside_the_session_are_skipped() -> None:
    events = frame([{"event": "add", "order_id": 1, "price_ticks": 1000000, "qty": 10}])
    builder = convert_events(events.with_columns(pl.col("ts_exchange") - 1), DATE)
    assert builder.records == []
    assert builder.stats.skipped_session == 1


def test_seq_sets_the_order_even_when_rows_are_shuffled() -> None:
    events = frame(
        [
            {"event": "add", "order_id": 1, "side": 0, "price_ticks": 1000000, "qty": 10},
            {"event": "delete", "order_id": 1},
        ]
    )
    builder = convert_events(events.reverse(), DATE)
    assert builder.stats.dropped_unknown_id == 0
    assert len(builder.records) == 2


def test_unknown_event_is_an_error() -> None:
    with pytest.raises(ValueError, match="unknown book_l3 event"):
        convert_events(frame([{"event": "halt", "order_id": 1}]), DATE)


def test_replay_config_follows_dq_07() -> None:
    builder = ReplayBuilder()
    builder.add(1, 0, 1000000, 1)
    builder.add(2, 1, 2000000, 1)
    builder.add(3, 1, 2000100, 1)
    assert replay_config(builder.stats) == {
        "price_min": 8999,
        "price_max": 21002,
        "max_orders": 8,
    }


def test_log_header_and_records_round_trip() -> None:
    builder = ReplayBuilder()
    builder.add(1, 0, 1000000, 1)
    out = io.BytesIO()
    write_log(out, builder.records)
    data = out.getvalue()
    assert len(data) == 64 + 32
    magic, version, record_size, _, tick, count, seed, source, reserved = HEADER.unpack(data[:64])
    assert (magic, version, record_size, count, seed, source) == (b"LLMEMLOG", 1, 32, 1, 0, 1)
    assert tick == 10_000_000
    assert reserved == bytes(23)
    assert struct.unpack("<B", data[64:65]) == (1,)

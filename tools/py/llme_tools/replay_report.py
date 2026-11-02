"""Aggregates converter sidecars and llme-diff reports into one results file (AC-W-10).

Counts and rates only: nothing row-level from a licensed source reaches results/.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Annotated, Any

import typer

app = typer.Typer(add_completion=False)


def summarize(meta: dict[str, Any], diff: dict[str, Any]) -> dict[str, Any]:
    """One log's row: what was converted, what replay found, whether the engines agreed."""
    replay = meta.get("replay", {})
    row: dict[str, Any] = {
        "symbol": meta["symbol"],
        "msgs": replay.get("msgs"),
        "mapped": meta["mapped"],
        "dropped": meta["dropped"],
        "skipped": meta["skipped"],
        "max_live_orders": meta["max_live_orders"],
        "price_levels": meta["price_max"] - meta["price_min"] + 1,
        "crossed_add": replay.get("crossed_add"),
        "exec_contra_match_rate": replay.get("exec_contra_match_rate"),
        "exec_unfilled": replay.get("exec_unfilled"),
        "rejects": replay.get("rejects"),
        "cancel_rejects": replay.get("cancel_rejects"),
        "modify_rejects": replay.get("modify_rejects"),
        "output_hash": replay.get("output_hash"),
        "fills": diff["fills"],
        "mismatches": diff["mismatches"],
        "elapsed_s": diff["elapsed_s"],
    }
    if "order_messages" in meta:
        row["order_messages"] = meta["order_messages"]
    for key in ("top_of_book", "one_step_top_of_book"):
        if key in meta:
            row[key] = meta[key]
    return row


def dropped_fraction(row: dict[str, Any]) -> float:
    dropped = sum(row["dropped"].values())
    total = dropped + sum(row["mapped"].values())
    return dropped / total if total else 0.0


@app.command()
def main(
    mlog_dir: Annotated[Path, typer.Option(help="Directory of .mlog and .meta.json files")],
    diff_dir: Annotated[Path, typer.Option(help="Directory of llme-diff reports")],
    routes_agree: Annotated[int, typer.Option(help="Symbols where both ITCH routes agree")],
    routes_differ: Annotated[int, typer.Option(help="Symbols where they differ")],
    routes_missing: Annotated[int, typer.Option(help="Symbols with only one route")],
    out: Annotated[Path, typer.Option(help="Output JSON")],
) -> None:
    """Write the real-flow summary."""
    groups: dict[str, list[dict[str, Any]]] = {"itch": [], "book_l3": [], "lobster": []}
    for path in sorted(mlog_dir.glob("*.meta.json")):
        name = path.name.removesuffix(".meta.json")
        diff = json.loads((diff_dir / f"{name}.json").read_text())
        meta = json.loads(path.read_text())
        kind = next(key for key in groups if name.startswith(key))
        groups[kind].append(summarize(meta, diff))

    itch = sorted(groups["itch"], key=lambda r: -r.get("order_messages", 0))
    for rank, row in enumerate(itch, start=1):
        row["rank"] = rank
    for row in itch + groups["lobster"]:
        row["dropped_fraction"] = dropped_fraction(row)
    logs = groups["itch"] + groups["book_l3"] + groups["lobster"]
    report = {
        "logs": len(logs),
        "messages": sum(row["msgs"] or 0 for row in logs),
        "mismatches": sum(row["mismatches"] for row in logs),
        "itch_routes": {"agree": routes_agree, "differ": routes_differ, "missing": routes_missing},
        "itch": itch,
        "book_l3": [
            {key: row[key] for key in ("symbol", "msgs", "mismatches", "output_hash")}
            for row in groups["book_l3"]
        ],
        "lobster": groups["lobster"],
    }
    out.write_text(json.dumps(report, indent=1) + "\n")
    typer.echo(f"{report['logs']} logs, {report['mismatches']} mismatches, {out}")

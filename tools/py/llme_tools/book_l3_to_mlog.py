"""Canonical book_l3 Parquet to .mlog converter (FR-21). Implemented in M5."""

import typer

app = typer.Typer(add_completion=False)


@app.command()
def main() -> None:
    """Convert book_l3 events to an input log."""
    raise typer.Exit(code=2)

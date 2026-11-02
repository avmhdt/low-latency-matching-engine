"""Report figures from results/ (spec section 14). Implemented in M6."""

import typer

app = typer.Typer(add_completion=False)


@app.command()
def main() -> None:
    """Regenerate the figures under results/figures/."""
    raise typer.Exit(code=2)

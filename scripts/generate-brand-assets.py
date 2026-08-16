#!/usr/bin/env python3
"""Generate Windows and Web branding assets from the canonical PNG logo."""

from pathlib import Path
import sys

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "branding" / "hdrbridge-logo-source.png"


def canonical_logo(source: Image.Image) -> Image.Image:
    rgba = source.convert("RGBA")
    alpha = rgba.getchannel("A")
    # Ignore isolated, low-coverage pixels left around the generated artwork.
    threshold = alpha.point(lambda value: 255 if value >= 16 else 0)
    width, height = rgba.size
    rows = []
    cols = []
    pixels = threshold.load()
    row_coverage = max(16, width // 40)
    col_coverage = max(16, height // 40)
    for y in range(height):
        if sum(1 for x in range(width) if pixels[x, y]) >= row_coverage:
            rows.append(y)
    for x in range(width):
        if sum(1 for y in range(height) if pixels[x, y]) >= col_coverage:
            cols.append(x)
    if not rows or not cols:
        raise RuntimeError("logo artwork could not be located")

    left, top, right, bottom = cols[0], rows[0], cols[-1] + 1, rows[-1] + 1
    artwork = rgba.crop((left, top, right, bottom))
    side = max(artwork.size)
    padding = max(1, round(side * 0.08))
    canvas_side = side + padding * 2
    canvas = Image.new("RGBA", (canvas_side, canvas_side), (0, 0, 0, 0))
    canvas.alpha_composite(
        artwork,
        ((canvas_side - artwork.width) // 2, (canvas_side - artwork.height) // 2),
    )
    return canvas


def resized(master: Image.Image, size: int) -> Image.Image:
    return master.resize((size, size), Image.Resampling.LANCZOS)


def save_png(master: Image.Image, size: int, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    resized(master, size).save(destination, "PNG", optimize=True, compress_level=9)


def main() -> int:
    source_path = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else SOURCE
    if not source_path.is_file():
        raise FileNotFoundError(source_path)

    with Image.open(source_path) as source:
        master = canonical_logo(source)

    save_png(master, 1024, ROOT / "assets" / "branding" / "hdrbridge-logo.png")
    save_png(master, 512, ROOT / "web" / "public" / "icon-512.png")
    save_png(master, 192, ROOT / "web" / "public" / "icon-192.png")
    save_png(master, 180, ROOT / "web" / "public" / "apple-touch-icon.png")
    save_png(master, 32, ROOT / "web" / "public" / "favicon-32x32.png")

    ico_sizes = [(16, 16), (20, 20), (24, 24), (32, 32), (40, 40),
                 (48, 48), (64, 64), (128, 128), (256, 256)]
    desktop_icon = ROOT / "desktop" / "assets" / "hdrbridge.ico"
    desktop_icon.parent.mkdir(parents=True, exist_ok=True)
    resized(master, 256).save(desktop_icon, "ICO", sizes=ico_sizes)
    favicon = ROOT / "web" / "public" / "favicon.ico"
    resized(master, 64).save(favicon, "ICO", sizes=[(16, 16), (32, 32), (48, 48), (64, 64)])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

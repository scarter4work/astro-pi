"""Crawl or sync the gallery position index.

    python -m autocontrast.db.discover sync
    python -m autocontrast.db.discover sync --gallery eso --max-pages 3

A cold run enumerates thousands of detail pages at one request per second, so expect it
to take a while. It is resumable: re-running skips everything already indexed.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
from pathlib import Path

from .crawl import DriftError, PoliteFetcher, sync_gallery
from .gallery import GALLERIES
from .index import GalleryIndex

DEFAULT_INDEX = Path("data/gallery_index.sqlite")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m autocontrast.db.discover")
    parser.add_argument("command", choices=["sync"])
    parser.add_argument("--gallery", choices=sorted(GALLERIES), action="append",
                        help="gallery to sync; repeatable. Default: all.")
    parser.add_argument("--index", type=Path, default=DEFAULT_INDEX)
    parser.add_argument("--max-pages", type=int, default=None,
                        help="stop after N listing pages (useful for a smoke run)")
    args = parser.parse_args(argv)

    args.index.parent.mkdir(parents=True, exist_ok=True)
    index = GalleryIndex(args.index)
    fetcher = PoliteFetcher()
    now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    status = 0
    try:
        for key in (args.gallery or sorted(GALLERIES)):
            try:
                report = sync_gallery(index, GALLERIES[key], fetcher, now_utc=now,
                                      max_pages=args.max_pages)
                print(report.detail)
            except DriftError as exc:
                # Loud, and non-zero exit: a redesign must not look like a quiet no-op.
                print(f"DRIFT: {exc}")
                status = 1
        print(f"index now holds {index.count()} entries at {args.index}")
    finally:
        index.close()
    return status


if __name__ == "__main__":
    raise SystemExit(main())

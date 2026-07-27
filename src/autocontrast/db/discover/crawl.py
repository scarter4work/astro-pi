"""Crawl and incremental sync for the gallery position index (§5.2.1).

All network access is behind the :class:`Fetcher` protocol so the orchestration is
testable offline. Politeness is not optional: these are free public archives being
enumerated, so requests are serialized at one per second with a descriptive
User-Agent, backoff, and a hard per-run cap.
"""

from __future__ import annotations

import random
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Protocol

from .gallery import (
    GalleryConfig,
    detail_url,
    parse_detail,
    parse_listing,
    parse_result_total,
    search_url,
)
from .index import GalleryIndex

DEFAULT_USER_AGENT = (
    "AutoContrast/0.1 "
    "(+https://github.com/scarter4work/autocontrast; scarter4work@yahoo.com)"
)
MAX_IMAGE_BYTES = 256 * 1024 * 1024   # the 18000x18000 Orion mosaic fits comfortably
MAX_REQUESTS_PER_RUN = 8000           # full cold crawl of both galleries, with headroom
MIN_REQUEST_INTERVAL_S = 1.0
DRIFT_THRESHOLD = 0.5                 # >50% unparseable detail pages on one listing page
SYNC_SAFETY_DAYS = 7
RESULTS_PER_PAGE = 50
EPOCH_YEAR = 1990                     # "everything": both archives postdate this


class DriftError(RuntimeError):
    """Raised when detail pages stop parsing — a site redesign, not missing data.

    Loud by design (§12): a quietly-empty index would read as "nothing to discover".
    """


class RequestBudgetExceeded(RuntimeError):
    """Raised when a run exceeds ``MAX_REQUESTS_PER_RUN``."""


class Fetcher(Protocol):
    def get_text(self, url: str) -> str: ...
    def get_bytes(self, url: str, *, max_bytes: int) -> bytes: ...


class PoliteFetcher:
    """Rate-limited, retrying, capped HTTP client built on ``urllib.request``."""

    def __init__(
        self,
        user_agent: str = DEFAULT_USER_AGENT,
        min_interval_s: float = MIN_REQUEST_INTERVAL_S,
        max_requests: int = MAX_REQUESTS_PER_RUN,
        max_retries: int = 3,
    ):
        self.user_agent = user_agent
        self.min_interval_s = min_interval_s
        self.max_requests = max_requests
        self.max_retries = max_retries
        self._requests = 0
        self._last_request_at = 0.0

    def _throttle(self) -> None:
        if self._requests >= self.max_requests:
            raise RequestBudgetExceeded(
                f"Request budget of {self.max_requests} exhausted; refusing to continue."
            )
        elapsed = time.monotonic() - self._last_request_at
        # Jitter avoids a metronomic request pattern.
        wait = self.min_interval_s + random.uniform(0.0, 0.25) - elapsed
        if wait > 0:
            time.sleep(wait)
        self._last_request_at = time.monotonic()
        self._requests += 1

    def _open(self, url: str):
        request = urllib.request.Request(url, headers={"User-Agent": self.user_agent})
        for attempt in range(self.max_retries + 1):
            self._throttle()
            try:
                return urllib.request.urlopen(request, timeout=60)
            except urllib.error.HTTPError as exc:
                retry_after = exc.headers.get("Retry-After") if exc.headers else None
                if exc.code == 429 and retry_after and attempt < self.max_retries:
                    time.sleep(min(float(retry_after), 120.0))
                    continue
                if 500 <= exc.code < 600 and attempt < self.max_retries:
                    time.sleep(2.0 ** attempt)
                    continue
                raise
            except (urllib.error.URLError, TimeoutError):
                if attempt < self.max_retries:
                    time.sleep(2.0 ** attempt)
                    continue
                raise
        raise RuntimeError(f"unreachable retry state for {url}")

    def get_text(self, url: str) -> str:
        with self._open(url) as response:
            return response.read().decode("utf-8", errors="replace")

    def get_bytes(self, url: str, *, max_bytes: int = MAX_IMAGE_BYTES) -> bytes:
        with self._open(url) as response:
            payload = response.read(max_bytes + 1)
        if len(payload) > max_bytes:
            raise ValueError(
                f"{url} exceeds the {max_bytes} byte cap; refusing to buffer it."
            )
        return payload


@dataclass
class CrawlReport:
    gallery: str
    listed: int
    indexed: int
    skipped: int
    failed: int
    detail: str


def crawl_gallery(
    index: GalleryIndex,
    cfg: GalleryConfig,
    fetcher: Fetcher,
    criteria: dict[str, str],
    *,
    max_pages: int | None = None,
    refresh: bool = False,
) -> CrawlReport:
    """Enumerate a gallery search and index every result's published metadata.

    Resumable: ids already present are skipped without a request unless ``refresh``.
    Aborts with :class:`DriftError` if detail pages stop parsing.
    """
    listed = indexed = skipped = failed = 0
    page = 1
    total: int | None = None
    seen_this_run: set[str] = set()

    while True:
        listing_doc = fetcher.get_text(search_url(cfg, criteria, page=page))
        if total is None:
            total = parse_result_total(listing_doc)
        items = parse_listing(listing_doc)
        if not items:
            break

        # Progress guard. Without a parsed result total the only other exits are an
        # empty listing and max_pages, so a gallery that echoes results for
        # out-of-range pages would be crawled until the request budget trips. A page
        # contributing no id unseen *this run* means no progress, so stop.
        #
        # Keyed on run-local ids, deliberately NOT on index membership: on a re-crawl
        # every id is already indexed, and stopping on that would abort at page 1 and
        # silently destroy resumability. It also dedupes ids that legitimately appear
        # on two pages when something is published mid-crawl.
        fresh = [item for item in items if item.id not in seen_this_run]
        if not fresh:
            break
        seen_this_run.update(item.id for item in fresh)
        listed += len(fresh)

        unparseable = 0
        considered = 0
        for item in fresh:
            if not refresh and index.has(cfg.key, item.id):
                skipped += 1
                continue
            considered += 1
            url = detail_url(cfg, item.id)
            entry = parse_detail(
                fetcher.get_text(url),
                entry_id=item.id, gallery=cfg.key, detail_url=url,
                width_px=item.width_px, height_px=item.height_px,
            )
            if not entry.parsed_ok:
                unparseable += 1
                failed += 1
                continue
            index.upsert(entry)
            indexed += 1

        # Only genuine parse breakage counts. A page full of artwork parses fine and
        # simply publishes no position — that is data absence, not drift.
        if considered and unparseable / considered > DRIFT_THRESHOLD:
            raise DriftError(
                f"{unparseable}/{considered} detail pages on page {page} of {cfg.key} "
                f"failed to parse (>{DRIFT_THRESHOLD:.0%}). The gallery markup has most "
                "likely changed; refusing to index a partial archive. Re-run "
                "tools/capture_gallery_fixtures.sh and fix the parsers."
            )

        if not cfg.results_paginated:
            break
        if max_pages is not None and page >= max_pages:
            break
        if total is not None and page * RESULTS_PER_PAGE >= total:
            break
        page += 1

    return CrawlReport(
        gallery=cfg.key, listed=listed, indexed=indexed, skipped=skipped, failed=failed,
        detail=(f"{cfg.key}: listed {listed}, indexed {indexed}, skipped {skipped}, "
                f"failed {failed}"),
    )


def sync_gallery(
    index: GalleryIndex,
    cfg: GalleryConfig,
    fetcher: Fetcher,
    *,
    now_utc: str,
    safety_days: int = SYNC_SAFETY_DAYS,
    max_pages: int | None = None,
) -> CrawlReport:
    """Incremental sync using the galleries' ``published_since`` filter.

    The window starts at the last successful sync minus ``safety_days``, covering late
    edits and backdated publication. ``now_utc`` is passed in rather than read from the
    clock so the behavior is testable.
    """
    last = index.get_sync_state(cfg.key)
    if last is None:
        since = datetime(EPOCH_YEAR, 1, 1, tzinfo=timezone.utc)
    else:
        since = (datetime.fromisoformat(last.replace("Z", "+00:00"))
                 - timedelta(days=safety_days))

    report = crawl_gallery(
        index, cfg, fetcher,
        {
            "published_since_year": str(since.year),
            "published_since_month": str(since.month),
            "published_since_day": str(since.day),
        },
        max_pages=max_pages,
    )
    index.set_sync_state(cfg.key, now_utc)
    return report

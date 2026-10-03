"""Archive discovery (§5.2.1): local positional index over professional gallery renders."""

from __future__ import annotations

from .crawl import CrawlReport, DriftError, PoliteFetcher, crawl_gallery, sync_gallery
from .discover import (
    AcquisitionOutcome,
    DiscoveryOutcome,
    acquire_reference,
    discover_reference,
)
from .gallery import GALLERIES, GalleryConfig, GalleryEntry
from .index import GalleryIndex

__all__ = [
    "GALLERIES", "GalleryConfig", "GalleryEntry", "GalleryIndex",
    "PoliteFetcher", "CrawlReport", "DriftError", "crawl_gallery", "sync_gallery",
    "AcquisitionOutcome", "DiscoveryOutcome", "acquire_reference", "discover_reference",
]

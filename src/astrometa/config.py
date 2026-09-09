from dataclasses import dataclass
from pathlib import Path

@dataclass(frozen=True)
class Config:
    archive_root: Path = Path("/archive/astro_data")
    live_root: Path = Path("/live/astro_data")
    db_path: Path = Path("/data/astro-metadata/store.sqlite")
    scratch_dir: Path = Path("/var/tmp/astrometa")
    astap_bin: Path = Path("/opt/astap/astap_cli")
    astap_db_dir: Path = Path("/opt/astap")

EXCLUDED_PATH_MARKERS = ("_dedup_quarantine", "@Recycle")
FINGERPRINT_SIZE = 16          # 16x16 grid -> 240-bit dHash

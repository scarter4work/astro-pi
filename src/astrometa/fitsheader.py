from pathlib import Path

BLOCK = 2880
CARD = 80
MAX_BLOCKS = 100          # 288 KB; a real header never approaches this


class FitsHeaderError(Exception):
    pass


def _parse_value(raw: str) -> str | int | float | bool:
    raw = raw.strip()
    if raw.startswith("'"):
        end = raw.find("'", 1)
        if end == -1:
            raise FitsHeaderError(f"unterminated string: {raw!r}")
        return raw[1:end].strip()
    raw = raw.split("/", 1)[0].strip()
    if raw in ("T", "F"):
        return raw == "T"
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw


def read_header(path: Path) -> dict[str, str | int | float | bool]:
    cards: dict[str, str | int | float | bool] = {}
    with open(path, "rb") as f:
        for _ in range(MAX_BLOCKS):
            block = f.read(BLOCK)
            if len(block) < BLOCK:
                raise FitsHeaderError(f"truncated header in {path}")
            for i in range(0, BLOCK, CARD):
                card = block[i:i + CARD].decode("ascii", errors="replace")
                key = card[:8].strip()
                if key == "END":
                    return cards
                if card[8:10] != "= ":
                    continue
                cards[key] = _parse_value(card[10:])
    raise FitsHeaderError(f"no END card within {MAX_BLOCKS} blocks in {path}")

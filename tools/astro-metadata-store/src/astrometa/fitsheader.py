from pathlib import Path

BLOCK = 2880
CARD = 80
MAX_BLOCKS = 100          # 288 KB; a real header never approaches this


class FitsHeaderError(Exception):
    pass


def _parse_value(raw: str) -> str | int | float | bool:
    raw = raw.strip()
    if raw.startswith("'"):
        # Find closing quote, handling FITS escaped quotes ('')
        i = 1
        while i < len(raw):
            if raw[i] == "'":
                # Check if this quote is escaped (doubled)
                if i + 1 < len(raw) and raw[i + 1] == "'":
                    # Escaped quote: skip both characters
                    i += 2
                else:
                    # This is the closing quote
                    value = raw[1:i]
                    # Unescape: '' -> '
                    value = value.replace("''", "'")
                    return value.strip()
            else:
                i += 1
        # No closing quote found
        raise FitsHeaderError(f"unterminated string: {raw!r}")
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
                try:
                    card = block[i:i + CARD].decode("ascii")
                except UnicodeDecodeError as e:
                    raise FitsHeaderError(
                        f"non-ASCII byte in header card in {path}: {e}"
                    ) from e
                key = card[:8].strip()
                if key == "END":
                    return cards
                if card[8:10] != "= ":
                    continue
                cards[key] = _parse_value(card[10:])
    raise FitsHeaderError(f"no END card within {MAX_BLOCKS} blocks in {path}")

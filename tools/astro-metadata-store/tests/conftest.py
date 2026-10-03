import pytest


def _card(key: str, value: str) -> bytes:
    return f"{key:<8}= {value:<70}"[:80].encode("ascii")


def build_fits_header(cards: dict[str, str], pad_to_blocks: int = 1) -> bytes:
    out = b"".join(_card(k, v) for k, v in cards.items())
    out += b"END".ljust(80)
    remainder = len(out) % 2880
    if remainder:
        out += b" " * (2880 - remainder)
    while len(out) < 2880 * pad_to_blocks:
        out += b" " * 2880
    return out


@pytest.fixture
def make_fits(tmp_path):
    def _make(name: str, cards: dict[str, str], pixels: bytes = b"") -> "Path":
        p = tmp_path / name
        p.write_bytes(build_fits_header(cards) + pixels)
        return p
    return _make

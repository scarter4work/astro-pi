### Task 9: Object naming and identity assertions

**Files:**
- Create: `src/astrometa/naming.py`
- Test: `tests/test_naming.py`

**Interfaces:**
- Consumes: `db`
- Produces:
  - `naming.canonicalise(name: str) -> str` — `M 42`, `m42`, `M42` all collapse to `M42`
  - `naming.upsert_object(conn, canonical, object_type, simbad_id=None, ra=None, dec=None) -> int`
  - `naming.add_alias(conn, object_id, alias, source) -> None`
  - `naming.assert_identity(conn, field_id, object_id, source) -> None`
  - `naming.CONFIDENCE: dict[str, str]` mapping source to confidence
  - `naming.may_drive_move(source: str) -> bool`

Confidence is the gate that makes the store safe to act on later: only `solved`, `propagated` and `manual` may ever move a file. `dirname` and `object_card` are recorded but never authorise a move.

- [ ] **Step 1: Write the failing test**

```python
# tests/test_naming.py
import pytest
from astrometa import db, naming

@pytest.mark.parametrize("raw,expected", [
    ("M 42", "M42"), ("M42", "M42"), ("m42", "M42"), ("  M  42 ", "M42"),
    ("NGC 7635", "NGC7635"), ("ngc7635", "NGC7635"),
    ("IC 1848", "IC1848"), ("Sh2-106", "SH2-106"), ("SH2-101", "SH2-101"),
])
def test_canonicalise(raw, expected):
    assert naming.canonicalise(raw) == expected

def test_canonicalise_leaves_proper_names_alone():
    assert naming.canonicalise("Triangulum Galaxy") == "Triangulum Galaxy"

def test_upsert_object_is_idempotent(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    a = naming.upsert_object(conn, "M42", "deepsky")
    b = naming.upsert_object(conn, "M42", "deepsky")
    assert a == b
    assert conn.execute("SELECT COUNT(*) FROM objects").fetchone()[0] == 1

def test_aliases_collapse_to_one_object(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M33", "deepsky")
    naming.add_alias(conn, oid, "Triangulum Galaxy", "dirname")
    naming.add_alias(conn, oid, "M 33", "dirname")
    naming.add_alias(conn, oid, "M 33", "dirname")     # duplicate is a no-op
    assert conn.execute("SELECT COUNT(*) FROM aliases").fetchone()[0] == 2

def test_confidence_gate_allows_only_trusted_sources():
    assert naming.may_drive_move("solved") is True
    assert naming.may_drive_move("propagated") is True
    assert naming.may_drive_move("manual") is True
    assert naming.may_drive_move("object_card") is False
    assert naming.may_drive_move("dirname") is False

def test_assert_identity_records_confidence(tmp_path):
    conn = db.connect(tmp_path / "t.sqlite"); db.init_schema(conn)
    oid = naming.upsert_object(conn, "M42", "deepsky")
    fid = conn.execute("INSERT INTO fields (solve_source) "
                       "VALUES ('astap')").lastrowid
    naming.assert_identity(conn, fid, oid, "solved")
    row = conn.execute("SELECT source, confidence FROM "
                       "identity_assertions").fetchone()
    assert row == ("solved", "high")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `pytest tests/test_naming.py -v`
Expected: FAIL with `ModuleNotFoundError: No module named 'astrometa.naming'`

- [ ] **Step 3: Write minimal implementation**

```python
# src/astrometa/naming.py
import re
from datetime import datetime, timezone

CONFIDENCE = {
    "solved": "high",
    "propagated": "high",
    "manual": "authoritative",
    "object_card": "medium",
    "dirname": "low",
}
_MOVE_ALLOWED = {"solved", "propagated", "manual"}

_CATALOG = re.compile(
    r"^\s*(M|NGC|IC|SH2|LDN|LBN|B|VDB)\s*-?\s*([0-9]+)\s*$", re.IGNORECASE)

def canonicalise(name: str) -> str:
    if not name:
        return name
    m = _CATALOG.match(name)
    if not m:
        return name.strip()
    prefix = m.group(1).upper()
    number = m.group(2)
    sep = "-" if prefix == "SH2" else ""
    return f"{prefix}{sep}{number}"

def may_drive_move(source: str) -> bool:
    return source in _MOVE_ALLOWED

def upsert_object(conn, canonical, object_type,
                  simbad_id=None, ra=None, dec=None) -> int:
    conn.execute("""INSERT INTO objects (canonical_name, object_type,
        simbad_id, ra, dec) VALUES (?,?,?,?,?)
        ON CONFLICT(canonical_name) DO UPDATE SET
          object_type=excluded.object_type,
          simbad_id=COALESCE(excluded.simbad_id, objects.simbad_id)""",
        (canonical, object_type, simbad_id, ra, dec))
    conn.commit()
    return conn.execute("SELECT id FROM objects WHERE canonical_name=?",
                        (canonical,)).fetchone()[0]

def add_alias(conn, object_id: int, alias: str, source: str) -> None:
    conn.execute("INSERT OR IGNORE INTO aliases (object_id, alias, source) "
                 "VALUES (?,?,?)", (object_id, alias, source))
    conn.commit()

def assert_identity(conn, field_id: int, object_id: int, source: str) -> None:
    conn.execute("""INSERT INTO identity_assertions (field_id, object_id,
        source, confidence, asserted_at) VALUES (?,?,?,?,?)""",
        (field_id, object_id, source, CONFIDENCE[source],
         datetime.now(timezone.utc).isoformat()))
    if may_drive_move(source):
        conn.execute("UPDATE fields SET object_id=? WHERE id=?",
                     (object_id, field_id))
    conn.commit()
```

- [ ] **Step 4: Run test to verify it passes**

Run: `pytest tests/test_naming.py -v`
Expected: PASS, 16 tests

- [ ] **Step 5: Commit**

```bash
git add src/astrometa/naming.py tests/test_naming.py
git commit -m "feat: object canonicalisation, aliases, identity confidence gate"
```

---


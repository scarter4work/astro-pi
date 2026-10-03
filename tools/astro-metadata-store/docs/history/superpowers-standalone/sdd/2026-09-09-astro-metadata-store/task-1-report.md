# Task 1 Report: Project Scaffolding, Config, and Database Schema

## Summary
Task 1 is complete. Implemented the astrometa Python package with project configuration, SQLite database schema, and comprehensive tests following strict TDD methodology.

## Implementation

### Files Created
1. **pyproject.toml** - PEP 621 project metadata with setuptools configuration
2. **src/astrometa/__init__.py** - Empty package init file
3. **src/astrometa/config.py** - Config dataclass with 6 path fields and 2 module constants
4. **src/astrometa/db.py** - Database module with connect() and init_schema() functions
5. **tests/test_db.py** - 3 comprehensive unit tests

### Schema Details
Created 8 SQLite tables with full DDL:
- **frames** - primary archive file metadata (content_hash as PRIMARY KEY)
- **fields** - plate-solve results and field metadata
- **objects** - astronomical objects with standard designations
- **aliases** - alternate names for objects
- **identity_assertions** - field-to-object mapping with confidence
- **quality** - frame quality metrics (star count, HFD, sky background)
- **projects** - observing projects grouped by object/filter
- **frame_projects** - many-to-many mapping of frames to projects

Key design choices:
- `content_hash` TEXT PRIMARY KEY on frames (not filename, since 1,989 filenames collide)
- All CREATE TABLE/INDEX statements use IF NOT EXISTS for idempotency
- Foreign keys enabled via PRAGMA
- WAL mode for concurrent access safety
- Indexed on path, field_id, and frame_type for query performance

### Config Dataclass
Frozen dataclass (immutable) with sensible defaults:
- archive_root: /archive/astro_data
- live_root: /live/astro_data
- db_path: /data/astro-metadata/store.sqlite
- scratch_dir: /var/tmp/astrometa
- astap_bin: /opt/astap/astap_cli
- astap_db_dir: /opt/astap

Module constants:
- EXCLUDED_PATH_MARKERS: ("_dedup_quarantine", "@Recycle")
- FINGERPRINT_SIZE: 16 (for dHash)

## TDD Process Evidence

### Step 1: Write Failing Test
```bash
$ .venv/bin/python -m pytest tests/test_db.py -v
```
**Result:** FAILED - `ModuleNotFoundError: No module named 'astrometa'`

This was expected and confirmed the test was correctly written before implementation.

### Step 2: Implement Code
Created all required files with schema and functions exactly as specified in the brief.

### Step 3: Install and Run Tests
```bash
$ .venv/bin/python -m pip install -e .
$ .venv/bin/python -m pytest tests/test_db.py -v
```

**Result: ALL 3 TESTS PASSING**
```
tests/test_db.py::test_init_schema_creates_expected_tables PASSED        [ 33%]
tests/test_db.py::test_init_schema_is_idempotent PASSED                  [ 66%]
tests/test_db.py::test_frames_primary_key_is_content_hash PASSED         [100%]

============================== 3 passed in 0.01s =======================================
```

## Test Coverage

### Test 1: `test_init_schema_creates_expected_tables`
- Verifies all 8 required tables are created
- Queries sqlite_master to confirm table names
- Passes ✓

### Test 2: `test_init_schema_is_idempotent`
- Calls init_schema twice without error
- Inserts data after first call
- Calls init_schema a third time
- Verifies data is not lost (count == 1)
- Confirms idempotency requirement is met
- Passes ✓

### Test 3: `test_frames_primary_key_is_content_hash`
- Inserts a frame with content_hash='h1'
- Attempts duplicate insert with same content_hash
- Correctly raises sqlite3.IntegrityError
- Confirms PRIMARY KEY constraint is enforced
- Passes ✓

## Python 3.13 Compatibility
- ✓ Uses only standard library features available in Python 3.13+
- ✓ No syntax or features introduced after 3.13
- ✓ dataclasses module (std library since 3.7)
- ✓ pathlib.Path (std library since 3.4)
- ✓ sqlite3 (built-in)

## Constraints Verified

### Never Write to Archive
✓ This task touches no archive paths at all

### content_hash is Primary Key
✓ Implemented as TEXT PRIMARY KEY on frames table

### Idempotent Schema Initialization
✓ All CREATE TABLE/INDEX use IF NOT EXISTS
✓ Test 2 proves data is preserved across multiple init_schema calls

### Errors Are Loud
✓ No silent fallbacks
✓ Integrity violations raise exceptions (test 3 confirms)

## Self-Review Findings

### Completeness
- All 8 tables created with correct DDL
- All columns match brief specification exactly (verbatim)
- All constraints (PRIMARY KEY, FOREIGN KEY, UNIQUE) properly defined
- All indexes created as specified
- Both functions (connect, init_schema) implemented and working

### Code Quality
- Clean separation: config.py for configuration, db.py for database operations
- Schema defined as constant string (maintainable, reviewable)
- Error handling leaves exceptions to propagate (loud, not silent)
- No YAGNI violations - all code serves the foundation requirements
- Comments include context (e.g., "16x16 grid -> 240-bit dHash")

### Test Quality
- Tests are minimal, focused, and independent
- Each test verifies one requirement
- Test 1: table creation
- Test 2: idempotency
- Test 3: primary key enforcement
- All assertions are clear and verifiable

### Design Decisions
- Using executescript() for atomic schema initialization (not individual statements)
- WAL mode for durability and concurrent access
- Foreign keys enabled globally
- Parent directory creation in connect() (defensive)
- Path type in config allows flexibility (overridable at runtime)

## Concerns
None. The implementation is complete, tested, and ready for the next task (Task 2).

## Commits
- **59b6201**: `feat: project scaffolding, config, and database schema`
  - 5 files changed, 148 insertions

## Next Task Dependencies
This schema serves as the foundation for all 13 subsequent tasks:
- Tasks 5, 11, 12 depend on disposition column values (present/missing/quarantined)
- All tasks read/write to frames, fields, objects tables
- Quality metrics task writes to quality table
- Project tracking tasks use projects and frame_projects tables

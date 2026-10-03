# QE Camera-Database Runtime Updater — Design

**Date:** 2026-09-03
**Status:** Design (v5.0.0.0 scope — holds the release)
**Originating discussion:** Brainstorm session 2026-09-03
**Depends on:** `share/qe_database.json` (Task 16), sensor index (`e9e7a10`), `tools/import_qe_research.py`
**Related spec:** `2026-04-26-color-science-overhaul-design.md` §6.3 (QE source, override semantics)

---

## 1. Context and Motivation

The QE camera database ships inside the module's release package. That makes the data exactly as fresh as the last build, and no fresher.

PixInsight's own SPCC demonstrates where that ends: its filter database is fixed at build time, so it does not update. Users acquire hardware the database has never heard of and have no path to fix it short of a new release of the host application. NukeX inherits the identical failure mode the moment its camera roster is coupled to its release cadence — and NukeX's roster churns faster than SPCC's filter list, because vendors ship new bodies on existing sensors continuously (55 cameras across only 24 sensors today; IMX571 alone carries six).

The consequence is concrete and already visible. An unrecognised camera falls back to `generic_sony_imx_osc` and stamps `NUKEX_QE_CONFIDENCE = generic-fallback`, degrading the Q-solve for dual-narrowband work — the exact path the colour-science overhaul exists to make correct.

**This design decouples data freshness from release cadence.** The module gains the ability to fetch a newer, cryptographically signed camera database and install it without a rebuild. Once that mechanism exists, every subsequent roster improvement reaches users as data, needing no release at all.

## 2. Locked Decisions (from brainstorm)

| Question | Choice | Rationale |
|---|---|---|
| Where it runs | **Runtime updater in the module** | Shipping data in the signed update package was considered and rejected: it is the SPCC failure mode. Data freshness must not depend on release cadence. |
| Trust mechanism | **Vendored Ed25519 (TweetNaCl), detached signatures** | PCL exposes no public-key or signature-verification API (see §3). Signing is not optional: the payload feeds a scientific solve. |
| Release scope | **v5.0.0.0 — holds the release** | If the updater slips to a v5.0.1 that never ships, NukeX lands in the SPCC trap anyway. The escape hatch must exist in the first v5 release. |
| Tooling scope | **Updater + signing only** | The manufacturer-checking roster tool is deliberately *out* of v5.0.0.0. Once the updater ships, that tool's output is data and needs no release to reach users. |
| Trigger | **Interval check on interface open, install on consent** | Default 7 days, opt-out in settings. Never during a stack. Surfaces staleness without anyone remembering to look, while pixels never change without the user agreeing. |
| Reproducibility | **`NUKEX_QE_DB_VERSION` in the composed header** | A database that changes under the user is a reproducibility hazard; the header makes a changed result explainable rather than mysterious. |

## 3. The Constraint That Shaped the Trust Model

`pcl/Cryptography.h` provides `CryptographicHash` (MD5, SHA-1/224/256/384/512) and `Cipher` (AES-256). It provides **no public-key primitives and no signature verification.**

PixInsight's `--sign-module-file` / `--sign-xml-file` are command-line operations against an `.xssk` keyring. Runtime verification of `.xsgn` signatures happens inside PI's module loader and updater; it is not reachable from a module for an arbitrary downloaded file.

`pcl/NetworkTransfer.h` *does* supply what the fetch needs — `SetURL()`, `SetSSL( useSSL, forceSSL, verifyPeer, verifyHost )`, `Download()`. The transport is available; only the trust step is missing.

Therefore the module vendors an Ed25519 verifier. TweetNaCl is the chosen implementation: a single public-domain C file of roughly 700 lines, no external runtime dependency, widely audited, and needing only signature verification. Note the API shape: TweetNaCl exposes the *combined* `crypto_sign_open()` (signature prepended to the message), not libsodium's detached `crypto_sign_verify_detached()`. Verifying a detached signature therefore means concatenating `sig || body` into a scratch buffer and calling `crypto_sign_open()` on it -- a thin wrapper in `qe_update.cpp`, not a reason to prefer a heavier dependency. This is consistent with the project's standing preference for carrying heavyweight vendored dependencies over inventing lighter substitutes.

**TLS is not sufficient on its own.** It authenticates the host, not the payload, and leaves a compromised or mis-published repository able to feed arbitrary QE values into the Q-solve. Signature verification is what makes the payload trustworthy independent of where it came from.

## 4. Published Artifacts

Two signed files served from `repository/` via raw.githubusercontent, alongside the existing PixInsight repository:

```
repository/qe_manifest.json        + qe_manifest.json.sig
repository/qe_database.json        + qe_database.json.sig
```

`qe_manifest.json` is deliberately small, so the routine poll is cheap:

```json
{
  "schema_version": 1,
  "db_version": 14,
  "db_sha512": "<128 hex>",
  "db_bytes": 44225,
  "n_cameras": 61,
  "n_sensors": 26,
  "released_utc": "2026-09-10T00:00:00Z",
  "summary": "Adds 6 cameras (Player One Ares-C Pro, ...); adds sensor IMX676."
}
```

`db_version` is a monotonically increasing integer, independent of the module version. `schema_version` matches the database's own and gates compatibility.

**The manifest is signed as well as the database.** An unsigned manifest would let anyone able to serve files pin a client to an old version or forge the summary shown in the consent prompt. Both signatures are detached Ed25519 over the exact file bytes, base64-encoded. The digest is SHA-512 rather than SHA-256 deliberately: `nukex4_calibration` does not link PCL, so it cannot reach `pcl::SHA256`, and TweetNaCl already carries SHA-512 as `crypto_hash()`. Reusing it avoids vendoring a second hash for no gain.

Verification is belt-and-braces: the manifest signature must verify, *then* the downloaded database must satisfy both its own signature and the `db_sha512` the signed manifest names. The redundancy costs nothing and closes the gap where a valid-but-different signed database is substituted for the one the manifest describes.

## 5. Components and Boundaries

**`src/lib/calibration/qe_update.{hpp,cpp}` — `QEUpdater`.** All decision logic, no I/O of its own:

```cpp
struct FetchResult { bool ok; std::string body; std::string error; };

// Injected so the library never links PCL and tests never touch the network.
class Fetcher {
public:
    virtual ~Fetcher() = default;
    virtual FetchResult get( const std::string& url ) = 0;
};

enum class UpdateOutcome {
    UP_TO_DATE, AVAILABLE, INSTALLED,
    OFFLINE,            // silent, retry next interval
    BAD_SIGNATURE,      // loud: possible tampering
    SCHEMA_TOO_NEW,     // module is older than the published data
    REFUSED_ROLLBACK, DIGEST_MISMATCH, INSTALL_FAILED
};
```

The unit exposes `check()` (fetch + verify manifest, compare versions) and `install()` (fetch + verify database, atomic replace). Splitting them is what lets the consent prompt sit between: nothing is written before the user agrees.

**`third_party/tweetnacl/`** — vendored verifier, plus the thin detached-signature wrapper over `crypto_sign_open()` described in §3.

**Module layer** — `NetworkTransfer`-backed `Fetcher`, the interface glue in `NukeXInterface` (interval check on open, "Check now" button, opt-out checkbox, last-checked label), and the consent dialog.

**`tools/sign_qe_database.py`** — maintainer signing: given the private key, emits both files with their detached signatures and a regenerated manifest.

## 6. Storage, Precedence, Provenance

The downloaded database is written to `<user_data_root>/nukex4/qe_database.json` — the per-user 0700 directory `resolve_user_data_paths()` already creates for the Phase 8 rating DB and user model. This reuses an established location and sidesteps the root-owned `<PI>/share` problem entirely; a normal user cannot write there and must not need to.

Load precedence becomes:

1. `qe_overrides.json` selected in the interface — an explicit user act, always wins
2. `<user_data_root>/nukex4/qe_database.json` — downloaded, when present and newer
3. `<PI>/share/qe_database.json` — shipped, the permanent floor

The shipped copy is never modified or deleted, so a bad download is always recoverable by removing one file.

Installation is write-to-temp, `fsync`, `rename` within the same filesystem, so an interrupted write cannot leave a truncated database where a valid one was.

`NUKEX_QE_DB_VERSION` joins `NUKEX_QE_CONFIDENCE` on the composed window's keywords, recording the `db_version` actually used for the solve.

## 7. Failure Handling

Every failure preserves the working database and none of them block stacking.

| Condition | Behaviour |
|---|---|
| Offline, DNS failure, timeout, TLS verify failure | Silent. Record the attempt, retry next interval. A telescope laptop in a field is the normal case, not an error. |
| **Signature invalid (manifest or database)** | **Loud** Process Console warning naming the file. Never installed, never silently retried — this is possible tampering, not a network hiccup. |
| `db_sha512` mismatch against signed manifest | Discard, warn, treat as a failed check. |
| `schema_version` newer than the module supports | Warn once: the module is too old for this data. Do not install. |
| `db_version` older than what is installed | Refuse. Prevents a rollback attack and accidental republication of stale data. |
| Install path unwritable / disk full | Warn once, keep the current database. |
| A stack is running | No fetch is attempted at all. |

The user declining an offered version is remembered (`declined_version`), so the same prompt does not reappear every interval.

## 8. Update State

`<user_data_root>/nukex4/qe_update_state.json`:

```json
{
  "enabled": true,
  "interval_days": 7,
  "last_check_utc": "2026-09-03T18:00:00Z",
  "last_result": "UP_TO_DATE",
  "installed_db_version": 14,
  "declined_version": 0
}
```

A missing or malformed file is not an error — it is replaced with defaults, since losing the check timestamp costs one extra check and nothing else.

## 9. Testing

The injected `Fetcher` means the whole matrix runs offline and deterministically, against fixtures signed by a keypair generated in the test setup.

- Valid manifest + valid database → `INSTALLED`; file lands atomically; precedence prefers it over shipped
- Tampered database body → `BAD_SIGNATURE`, nothing written
- Tampered manifest (forged `db_version`) → `BAD_SIGNATURE`, no fetch of the database
- Database signed by the wrong key → `BAD_SIGNATURE`
- Valid signature, wrong `db_sha512` → `DIGEST_MISMATCH`
- `db_version` lower than installed → `REFUSED_ROLLBACK`
- `schema_version` above supported → `SCHEMA_TOO_NEW`
- Fetch failure → `OFFLINE`, state records the attempt, nothing written
- Install target unwritable → `INSTALL_FAILED`, shipped database still loads
- Interval logic: no check before the interval elapses; check after
- `declined_version` suppresses re-prompting for that version only
- Known-answer tests for the vendored verifier using published Ed25519 test vectors (RFC 8032), so a bad vendoring fails loudly rather than silently accepting everything

The vendored-verifier known-answer tests matter more than they look: a subtly broken verifier that returns success unconditionally would pass every other test in this list.

## 10. Explicitly Out of Scope

- **Manufacturer scraping** (`tools/refresh_camera_roster.py`) — deferred; its output is data and needs no release.
- **Automatic QE curve extraction** — vendors publish QE as plot images, never numbers. Digitising graphs would feed a scientific solve with values nobody verified. New *sensors* are flagged for manual research instead; new *cameras* on known sensors need only a roster row.
- **Filter database updates** — the same mechanism will extend to filters later; this design ships cameras only.

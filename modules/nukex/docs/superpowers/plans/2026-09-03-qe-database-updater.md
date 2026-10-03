# QE Camera-Database Runtime Updater Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let NukeX fetch and install a newer, cryptographically signed QE camera database at runtime, so the camera roster stops being frozen at build time.

**Architecture:** A pure-logic `QEUpdater` in `nukex4_calibration` owns every decision (version comparison, signature verification, digest check, atomic install) and performs no I/O of its own — it takes an injected `Fetcher`, so the whole failure matrix is testable with no network. Ed25519 verification comes from a vendored TweetNaCl. The module layer supplies a `NetworkTransfer`-backed `Fetcher`, the interface glue, and the consent dialog.

**Tech Stack:** C++17, TweetNaCl (vendored, public domain), nlohmann::json (already vendored), Catch2 v3 (already vendored), PCL `NetworkTransfer`, CMake.

**Spec:** `docs/superpowers/specs/2026-09-03-qe-database-updater-design.md`

## Global Constraints

- C++17 (`target_compile_features(... cxx_std_17)`), matching every existing `nukex4_*` library.
- `nukex4_calibration` **must not link PCL.** All PCL usage stays in `src/module/`. This is why the digest is SHA-512 (from TweetNaCl) and not `pcl::SHA256`.
- No network access in any unit test. Tests inject a fake `Fetcher`.
- Vendored dependencies carry their `LICENSE` and nothing else — no docs, tests, or examples (see the vendoring rule in CLAUDE.md).
- Every failure path preserves the working database and never blocks stacking.
- The shipped `<PI>/share/qe_database.json` is never modified or deleted.
- Test registration uses `nukex_add_test(<name> <source> <libs...>)` from `test/CMakeLists.txt:20`.
- Build: `cd build && cmake .. && make -j$(nproc)`. Tests: `cd build && ctest --output-on-failure`.
- Commit trailers on every commit:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
  ```

---

## File Structure

| File | Responsibility |
|---|---|
| `third_party/tweetnacl/tweetnacl.{c,h}`, `LICENSE` | Vendored Ed25519 + SHA-512 primitives |
| `src/lib/calibration/include/nukex/calibration/ed25519_verify.hpp` | Detached-signature and SHA-512 wrappers over TweetNaCl |
| `src/lib/calibration/src/ed25519_verify.cpp` | Their implementation, plus the `randombytes` link stub |
| `src/lib/calibration/include/nukex/calibration/qe_update.hpp` | `Fetcher`, `UpdateOutcome`, `QEManifest`, `QEUpdater` |
| `src/lib/calibration/src/qe_update.cpp` | Manifest parse, version policy, verify, atomic install |
| `src/lib/calibration/include/nukex/calibration/qe_update_state.hpp` | `QEUpdateState` load/save |
| `src/lib/calibration/src/qe_update_state.cpp` | Its JSON persistence |
| `src/module/QEFetcher.{h,cpp}` | `Fetcher` implemented over PCL `NetworkTransfer` |
| `src/module/NukeXInterface.{h,cpp}` | Interval check on open, "Check now", opt-out, consent dialog |
| `src/module/NukeXInstance.cpp` | Path precedence + `NUKEX_QE_DB_VERSION` keyword |
| `tools/sign_qe_database.py` | Maintainer signing + manifest generation |
| `test/unit/calibration/test_ed25519_verify.cpp` | RFC 8032 known-answer vectors |
| `test/unit/calibration/test_qe_update.cpp` | Full failure matrix with a fake `Fetcher` |
| `test/unit/calibration/test_qe_update_state.cpp` | State round-trip and corruption tolerance |

---

## Task 1: Vendor TweetNaCl and wrap it

**Files:**
- Create: `third_party/tweetnacl/tweetnacl.c`, `third_party/tweetnacl/tweetnacl.h`, `third_party/tweetnacl/LICENSE`
- Create: `src/lib/calibration/include/nukex/calibration/ed25519_verify.hpp`
- Create: `src/lib/calibration/src/ed25519_verify.cpp`
- Create: `test/unit/calibration/test_ed25519_verify.cpp`
- Modify: `src/lib/calibration/CMakeLists.txt`, `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `nukex::ed25519_verify_detached(sig, sig_len, msg, msg_len, pubkey) -> bool` and `nukex::sha512_hex(data, len) -> std::string`.

**Critical detail:** `tweetnacl.c` declares `extern void randombytes(unsigned char*, unsigned long long);` and will not link without it. Verification never calls it, so provide a stub that aborts loudly — if it is ever reached, that is a bug, not something to paper over with weak randomness.

**Second critical detail:** TweetNaCl has no `crypto_sign_verify_detached`. It exposes the combined `crypto_sign_open(m, &mlen, sm, smlen, pk)`, where `sm` is `signature || message`. The wrapper builds that buffer.

- [ ] **Step 1: Vendor the sources**

Fetch `tweetnacl.c` and `tweetnacl.h` from https://tweetnacl.cr.yp.to/ (public domain). Write `third_party/tweetnacl/LICENSE` containing the TweetNaCl public-domain dedication. Vendor nothing else.

Verify: `wc -l third_party/tweetnacl/tweetnacl.c` (expect roughly 700-900 lines) and `grep -c crypto_sign_open third_party/tweetnacl/tweetnacl.h` (expect ≥ 1).

- [ ] **Step 2: Write the failing test**

`test/unit/calibration/test_ed25519_verify.cpp`. The vectors are RFC 8032 §7.1 TEST 2 (a one-byte message), written as raw bytes so a bad vendoring cannot pass by coincidence.

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/calibration/ed25519_verify.hpp"

#include <string>
#include <vector>

using namespace nukex;

// RFC 8032 section 7.1, TEST 2.
static std::vector<unsigned char> hex_to_bytes(const std::string& h) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

static const char* kPub =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const char* kSig =
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
    "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
static const unsigned char kMsg[1] = { 0x72 };

TEST_CASE("ed25519: a genuine RFC 8032 signature verifies", "[ed25519]") {
    auto pub = hex_to_bytes(kPub);
    auto sig = hex_to_bytes(kSig);
    REQUIRE(ed25519_verify_detached(sig.data(), sig.size(), kMsg, sizeof(kMsg), pub.data()));
}

TEST_CASE("ed25519: a flipped message byte fails", "[ed25519]") {
    auto pub = hex_to_bytes(kPub);
    auto sig = hex_to_bytes(kSig);
    const unsigned char bad[1] = { 0x73 };
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), sig.size(), bad, sizeof(bad), pub.data()));
}

TEST_CASE("ed25519: a flipped signature byte fails", "[ed25519]") {
    auto pub = hex_to_bytes(kPub);
    auto sig = hex_to_bytes(kSig);
    sig[0] ^= 0x01;
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), sig.size(), kMsg, sizeof(kMsg), pub.data()));
}

TEST_CASE("ed25519: a wrong-length signature is rejected without reading past it", "[ed25519]") {
    auto pub = hex_to_bytes(kPub);
    auto sig = hex_to_bytes(kSig);
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), 63, kMsg, sizeof(kMsg), pub.data()));
}

TEST_CASE("sha512_hex matches the published empty-string digest", "[ed25519]") {
    REQUIRE(sha512_hex(reinterpret_cast<const unsigned char*>(""), 0) ==
            "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
            "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}
```

- [ ] **Step 2a: Register the test**

In `test/CMakeLists.txt`, after line 51 (`test_channel_decomposer`), add:

```cmake
nukex_add_test(test_ed25519_verify unit/calibration/test_ed25519_verify.cpp nukex4_calibration nukex4_io nukex4_core)
```

- [ ] **Step 3: Run it to confirm it fails**

Run: `cd build && cmake .. && make test_ed25519_verify 2>&1 | tail -5`
Expected: compile FAILS — `nukex/calibration/ed25519_verify.hpp: No such file or directory`.

- [ ] **Step 4: Write the header**

`src/lib/calibration/include/nukex/calibration/ed25519_verify.hpp`:

```cpp
#ifndef NUKEX_CALIBRATION_ED25519_VERIFY_HPP
#define NUKEX_CALIBRATION_ED25519_VERIFY_HPP

#include <cstddef>
#include <string>

namespace nukex {

// Ed25519 public keys are 32 bytes; detached signatures are 64.
inline constexpr std::size_t kEd25519PublicKeyBytes = 32;
inline constexpr std::size_t kEd25519SignatureBytes = 64;

// Verifies a DETACHED Ed25519 signature. TweetNaCl only offers the combined
// crypto_sign_open(), where the signature is prepended to the message, so
// this builds "sig || msg" in a scratch buffer and calls that. Returns false
// (never throws, never aborts) for any bad input, including a signature of
// the wrong length.
bool ed25519_verify_detached(const unsigned char* sig, std::size_t sig_len,
                             const unsigned char* msg, std::size_t msg_len,
                             const unsigned char* public_key);

// Lowercase hex SHA-512 of the buffer, via TweetNaCl's crypto_hash. Used for
// the manifest's db_sha512; SHA-512 rather than SHA-256 because this library
// must not link PCL and TweetNaCl already carries SHA-512.
std::string sha512_hex(const unsigned char* data, std::size_t len);

} // namespace nukex

#endif
```

- [ ] **Step 5: Write the implementation**

`src/lib/calibration/src/ed25519_verify.cpp`:

```cpp
#include "nukex/calibration/ed25519_verify.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include "tweetnacl.h"

// TweetNaCl declares randombytes() for key GENERATION. Verification never
// calls it. Providing weak randomness "just to link" would be a trap for
// whoever later adds signing here, so fail loudly instead.
void randombytes(unsigned char*, unsigned long long) {
    std::fprintf(stderr,
                 "nukex: randombytes() called -- this build vendors TweetNaCl "
                 "for verification only and has no CSPRNG wired up.\n");
    std::abort();
}
}

namespace nukex {

bool ed25519_verify_detached(const unsigned char* sig, std::size_t sig_len,
                             const unsigned char* msg, std::size_t msg_len,
                             const unsigned char* public_key) {
    if (sig == nullptr || public_key == nullptr) return false;
    if (sig_len != kEd25519SignatureBytes) return false;
    if (msg == nullptr && msg_len != 0) return false;

    // crypto_sign_open() wants signature || message, and writes a plaintext
    // of at most the same length.
    std::vector<unsigned char> signed_message(sig_len + msg_len);
    std::copy(sig, sig + sig_len, signed_message.begin());
    if (msg_len > 0) std::copy(msg, msg + msg_len, signed_message.begin() + sig_len);

    std::vector<unsigned char> plain(signed_message.size());
    unsigned long long plain_len = 0;

    return crypto_sign_open(plain.data(), &plain_len,
                            signed_message.data(),
                            static_cast<unsigned long long>(signed_message.size()),
                            public_key) == 0;
}

std::string sha512_hex(const unsigned char* data, std::size_t len) {
    unsigned char digest[64];
    crypto_hash(digest, data, static_cast<unsigned long long>(len));

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(digest) * 2);
    for (unsigned char b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

} // namespace nukex
```

- [ ] **Step 6: Wire the build**

In `src/lib/calibration/CMakeLists.txt`, change the `add_library` block to:

```cmake
add_library(nukex4_calibration STATIC
    src/qe_database.cpp
    src/channel_decomposer.cpp
    src/ed25519_verify.cpp
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
)
```

and after the existing `target_include_directories` block add:

```cmake
# TweetNaCl is PRIVATE: it is an implementation detail of ed25519_verify.cpp
# and no public header exposes its types.
target_include_directories(nukex4_calibration
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/tweetnacl
)

# TweetNaCl is C89 and trips -Wall on sign conversions inside its own bignum
# code. Silence warnings for that translation unit only, never project-wide.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
    PROPERTIES COMPILE_OPTIONS "-w"
)
```

- [ ] **Step 7: Run the tests to confirm they pass**

Run: `cd build && cmake .. && make test_ed25519_verify -j$(nproc) && ./test/test_ed25519_verify "[ed25519]"`
Expected: `All tests passed (5 assertions in 5 test cases)`.

- [ ] **Step 8: Confirm the whole suite is still green**

Run: `cd build && ctest 2>&1 | tail -4`
Expected: `100% tests passed` with the count one higher than before.

- [ ] **Step 9: Commit**

```bash
git add third_party/tweetnacl src/lib/calibration test/unit/calibration/test_ed25519_verify.cpp test/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(calibration): vendor TweetNaCl for Ed25519 verification and SHA-512

PCL exposes hashes and AES only -- no public-key primitives -- and PI's
.xsgn verification is not reachable from a module for arbitrary files, so
verifying a downloaded database needs a vendored verifier.

Two details worth knowing. TweetNaCl has no crypto_sign_verify_detached;
it offers the combined crypto_sign_open() over "signature || message", so
the wrapper builds that buffer. And tweetnacl.c declares randombytes() for
key generation, which will not link without a definition -- verification
never calls it, so the stub aborts rather than supplying weak randomness
that would quietly become a trap for whoever adds signing later.

SHA-512 comes along free with TweetNaCl and is used for the manifest
digest, so nukex4_calibration needs no second hash and still links no PCL.

Verified against RFC 8032 section 7.1 TEST 2 vectors, including flipped
message byte, flipped signature byte, and a truncated signature -- a
verifier that returns true unconditionally fails these.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Task 2: `QEUpdater::check()` — manifest fetch, verify, version policy

**Files:**
- Create: `src/lib/calibration/include/nukex/calibration/qe_update.hpp`
- Create: `src/lib/calibration/src/qe_update.cpp`
- Create: `test/unit/calibration/test_qe_update.cpp`
- Modify: `src/lib/calibration/CMakeLists.txt`, `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `nukex::ed25519_verify_detached`, `nukex::sha512_hex` (Task 1).
- Produces: `Fetcher`, `FetchResult`, `UpdateOutcome`, `QEManifest`, `QEUpdater::check()`.

- [ ] **Step 1: Write the header**

`src/lib/calibration/include/nukex/calibration/qe_update.hpp`:

```cpp
#ifndef NUKEX_CALIBRATION_QE_UPDATE_HPP
#define NUKEX_CALIBRATION_QE_UPDATE_HPP

#include <string>

namespace nukex {

struct FetchResult {
    bool        ok = false;
    std::string body;
    std::string error;
};

// Injected so nukex4_calibration never links PCL and tests never touch the
// network. The module supplies a NetworkTransfer-backed implementation.
class Fetcher {
public:
    virtual ~Fetcher() = default;
    virtual FetchResult get(const std::string& url) = 0;
};

enum class UpdateOutcome {
    UP_TO_DATE,
    AVAILABLE,          // newer version verified; nothing written yet
    INSTALLED,
    OFFLINE,            // silent; retry next interval
    BAD_SIGNATURE,      // loud: possible tampering
    SCHEMA_TOO_NEW,     // published data is newer than this module understands
    REFUSED_ROLLBACK,
    DIGEST_MISMATCH,
    MALFORMED_MANIFEST,
    INSTALL_FAILED
};

const char* to_string(UpdateOutcome o);

struct QEManifest {
    int         schema_version = 0;
    int         db_version     = 0;
    std::string db_sha512;
    long long   db_bytes       = 0;
    int         n_cameras      = 0;
    int         n_sensors      = 0;
    std::string released_utc;
    std::string summary;
};

struct CheckResult {
    UpdateOutcome outcome = UpdateOutcome::OFFLINE;
    QEManifest    manifest;      // populated only when outcome is AVAILABLE
    std::string   detail;        // human-readable, for the Process Console
};

class QEUpdater {
public:
    // base_url has no trailing slash; the updater appends the file names.
    // public_key must point at kEd25519PublicKeyBytes bytes that outlive this.
    QEUpdater(Fetcher& fetcher, std::string base_url, const unsigned char* public_key);

    // The highest schema_version this build can consume.
    static constexpr int kSupportedSchemaVersion = 1;

    // Fetches and verifies the manifest, then compares against
    // installed_db_version. Writes nothing.
    CheckResult check(int installed_db_version) const;

private:
    Fetcher&             fetcher_;
    std::string          base_url_;
    const unsigned char* public_key_;
};

} // namespace nukex

#endif
```

- [ ] **Step 2: Write the failing tests**

`test/unit/calibration/test_qe_update.cpp`. The fake `Fetcher` is a map from URL to `FetchResult`, and fixtures are signed with a keypair generated once in the test binary. Because TweetNaCl's `randombytes` aborts in this build, the tests use a **fixed** keypair generated offline and pasted in as hex — deterministic, and it exercises the same code path.

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_update.hpp"
#include "nukex/calibration/ed25519_verify.hpp"

#include <map>
#include <string>
#include <vector>

using namespace nukex;

// A fixed Ed25519 keypair, generated offline for tests only. The secret half
// is here so fixtures can be signed deterministically; it protects nothing.
// Generate replacements with:
//   python3 -c "import nacl.signing,binascii; k=nacl.signing.SigningKey.generate(); \
//               print(binascii.hexlify(bytes(k)).decode()); \
//               print(binascii.hexlify(bytes(k.verify_key)).decode())"
static const char* kTestPub = "<64 hex chars — fill from the generator above>";
static const char* kTestSec = "<128 hex chars — fill from the generator above>";

static std::vector<unsigned char> unhex(const std::string& h);
static std::string sign_b64(const std::string& body);   // signs with kTestSec

class FakeFetcher : public Fetcher {
public:
    std::map<std::string, FetchResult> responses;
    std::vector<std::string>           requested;

    FetchResult get(const std::string& url) override {
        requested.push_back(url);
        auto it = responses.find(url);
        if (it == responses.end()) return { false, "", "not reachable" };
        return it->second;
    }
};

static const char* kBase = "https://example.invalid/repo";

static std::string manifest_json(int db_version, int schema_version,
                                 const std::string& sha512) {
    return std::string("{\"schema_version\":") + std::to_string(schema_version) +
           ",\"db_version\":" + std::to_string(db_version) +
           ",\"db_sha512\":\"" + sha512 + "\"" +
           ",\"db_bytes\":10,\"n_cameras\":61,\"n_sensors\":26"
           ",\"released_utc\":\"2026-09-10T00:00:00Z\""
           ",\"summary\":\"Adds 6 cameras.\"}";
}

static void serve_manifest(FakeFetcher& f, const std::string& json) {
    f.responses[std::string(kBase) + "/qe_manifest.json"]     = { true, json, "" };
    f.responses[std::string(kBase) + "/qe_manifest.json.sig"] = { true, sign_b64(json), "" };
}

TEST_CASE("QEUpdater: a newer signed manifest reports AVAILABLE", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    serve_manifest(f, manifest_json(14, 1, std::string(128, 'a')));

    QEUpdater up(f, kBase, pub.data());
    auto r = up.check(13);

    REQUIRE(r.outcome == UpdateOutcome::AVAILABLE);
    REQUIRE(r.manifest.db_version == 14);
    REQUIRE(r.manifest.n_cameras == 61);
}

TEST_CASE("QEUpdater: the same version reports UP_TO_DATE", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    serve_manifest(f, manifest_json(14, 1, std::string(128, 'a')));

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(14).outcome == UpdateOutcome::UP_TO_DATE);
}

TEST_CASE("QEUpdater: an older published version is refused as a rollback", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    serve_manifest(f, manifest_json(12, 1, std::string(128, 'a')));

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(14).outcome == UpdateOutcome::REFUSED_ROLLBACK);
}

TEST_CASE("QEUpdater: a forged manifest body fails signature verification", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string good = manifest_json(14, 1, std::string(128, 'a'));
    f.responses[std::string(kBase) + "/qe_manifest.json"] =
        { true, manifest_json(99, 1, std::string(128, 'a')), "" };   // body swapped
    f.responses[std::string(kBase) + "/qe_manifest.json.sig"] = { true, sign_b64(good), "" };

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::BAD_SIGNATURE);
}

TEST_CASE("QEUpdater: a schema newer than this build is refused, not installed", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    serve_manifest(f, manifest_json(14, 99, std::string(128, 'a')));

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::SCHEMA_TOO_NEW);
}

TEST_CASE("QEUpdater: an unreachable host is OFFLINE, not an error", "[qe_update]") {
    FakeFetcher f;                       // serves nothing
    auto pub = unhex(kTestPub);

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::OFFLINE);
}

TEST_CASE("QEUpdater: a malformed manifest body is reported, not thrown", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string junk = "{not json";
    f.responses[std::string(kBase) + "/qe_manifest.json"]     = { true, junk, "" };
    f.responses[std::string(kBase) + "/qe_manifest.json.sig"] = { true, sign_b64(junk), "" };

    QEUpdater up(f, kBase, pub.data());
    REQUIRE(up.check(13).outcome == UpdateOutcome::MALFORMED_MANIFEST);
}

TEST_CASE("QEUpdater: check() never requests the database", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    serve_manifest(f, manifest_json(14, 1, std::string(128, 'a')));

    QEUpdater up(f, kBase, pub.data());
    up.check(13);

    for (const auto& url : f.requested)
        REQUIRE(url.find("qe_database.json") == std::string::npos);
}
```

Before running, fill `kTestPub` / `kTestSec` using the command in the comment (`pip install pynacl` if needed), and implement `unhex` and `sign_b64` at the bottom of the file — `sign_b64` calls TweetNaCl's `crypto_sign` via a small `extern "C"` declaration and base64-encodes the leading 64 bytes.

- [ ] **Step 2a: Register the test**

In `test/CMakeLists.txt`, after the `test_ed25519_verify` line:

```cmake
nukex_add_test(test_qe_update unit/calibration/test_qe_update.cpp nukex4_calibration nukex4_io nukex4_core)
```

- [ ] **Step 3: Run to confirm failure**

Run: `cd build && cmake .. && make test_qe_update 2>&1 | tail -5`
Expected: link error — `undefined reference to nukex::QEUpdater::check`.

- [ ] **Step 4: Implement `qe_update.cpp`**

Implement `to_string`, base64 decoding, manifest parsing with `nlohmann::json` (wrapped in try/catch returning `MALFORMED_MANIFEST`), and `check()` in this order:

1. `GET <base>/qe_manifest.json` and `<base>/qe_manifest.json.sig`; any fetch failure returns `OFFLINE`.
2. Base64-decode the signature; a decode failure or wrong length returns `BAD_SIGNATURE`.
3. `ed25519_verify_detached` over the exact manifest bytes; failure returns `BAD_SIGNATURE`.
4. Parse; failure returns `MALFORMED_MANIFEST`.
5. `schema_version > kSupportedSchemaVersion` returns `SCHEMA_TOO_NEW`.
6. `db_version < installed` returns `REFUSED_ROLLBACK`; `==` returns `UP_TO_DATE`; `>` returns `AVAILABLE` with the manifest populated.

Order matters: verify the signature **before** parsing, so malformed input from an unauthenticated source is never fed to the JSON parser.

- [ ] **Step 5: Run to confirm the tests pass**

Run: `cd build && make test_qe_update -j$(nproc) && ./test/test_qe_update "[qe_update]"`
Expected: all 8 cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/lib/calibration test/unit/calibration/test_qe_update.cpp test/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(calibration): QEUpdater::check -- signed manifest fetch and version policy

check() fetches only the small manifest, so the routine poll costs one
request and no database transfer. The signature is verified BEFORE the
body is parsed, so unauthenticated input never reaches the JSON parser.

Version policy is explicit about the case that is easy to miss: a
published version OLDER than the installed one is refused rather than
ignored, because silently accepting it is a rollback attack.

Fetching is injected, so the whole failure matrix -- offline, forged body,
schema too new, malformed JSON -- is covered without touching a network.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Task 3: `QEUpdater::install()` — download, verify, atomic replace

**Files:**
- Modify: `src/lib/calibration/include/nukex/calibration/qe_update.hpp`, `src/lib/calibration/src/qe_update.cpp`
- Modify: `test/unit/calibration/test_qe_update.cpp`

**Interfaces:**
- Consumes: `CheckResult` and `QEManifest` from Task 2.
- Produces: `UpdateOutcome QEUpdater::install(const QEManifest&, const std::string& dest_path) const`.

- [ ] **Step 1: Add the failing tests**

Append to `test/unit/calibration/test_qe_update.cpp`. Each test writes into a unique temp directory and asserts on the file that results.

```cpp
#include <filesystem>
#include <fstream>
namespace fs = std::filesystem;

static std::string temp_dest(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("nukex_qe_update_" + std::string(tag) + "_" +
                  std::to_string(::getpid()));
    fs::create_directories(p);
    return (p / "qe_database.json").string();
}

static std::string read_file(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
}

static void serve_db(FakeFetcher& f, const std::string& body) {
    f.responses[std::string(kBase) + "/qe_database.json"] = { true, body, "" };
    f.responses[std::string(kBase) + "/qe_database.json.sig"] = { true, sign_b64(body), "" };
}

TEST_CASE("QEUpdater: a verified database installs atomically", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string db = "{\"schema_version\":1,\"cameras\":{},\"filters\":{}}";
    const std::string digest =
        sha512_hex(reinterpret_cast<const unsigned char*>(db.data()), db.size());
    serve_manifest(f, manifest_json(14, 1, digest));
    serve_db(f, db);

    QEUpdater up(f, kBase, pub.data());
    auto chk = up.check(13);
    REQUIRE(chk.outcome == UpdateOutcome::AVAILABLE);

    const std::string dest = temp_dest("ok");
    REQUIRE(up.install(chk.manifest, dest) == UpdateOutcome::INSTALLED);
    REQUIRE(read_file(dest) == db);
    // No scratch file survives a successful install.
    REQUIRE_FALSE(fs::exists(dest + ".tmp"));
}

TEST_CASE("QEUpdater: a tampered database body is not installed", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string db = "{\"schema_version\":1,\"cameras\":{},\"filters\":{}}";
    const std::string digest =
        sha512_hex(reinterpret_cast<const unsigned char*>(db.data()), db.size());
    serve_manifest(f, manifest_json(14, 1, digest));
    f.responses[std::string(kBase) + "/qe_database.json"] = { true, db + " ", "" };
    f.responses[std::string(kBase) + "/qe_database.json.sig"] = { true, sign_b64(db), "" };

    QEUpdater up(f, kBase, pub.data());
    auto chk = up.check(13);
    const std::string dest = temp_dest("tamper");
    REQUIRE(up.install(chk.manifest, dest) == UpdateOutcome::BAD_SIGNATURE);
    REQUIRE_FALSE(fs::exists(dest));
}

TEST_CASE("QEUpdater: a correctly signed database with the wrong digest is refused", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string db = "{\"schema_version\":1,\"cameras\":{},\"filters\":{}}";
    serve_manifest(f, manifest_json(14, 1, std::string(128, 'b')));  // digest of nothing
    serve_db(f, db);

    QEUpdater up(f, kBase, pub.data());
    auto chk = up.check(13);
    const std::string dest = temp_dest("digest");
    REQUIRE(up.install(chk.manifest, dest) == UpdateOutcome::DIGEST_MISMATCH);
    REQUIRE_FALSE(fs::exists(dest));
}

TEST_CASE("QEUpdater: an existing database survives a failed install", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string db = "{\"schema_version\":1,\"cameras\":{},\"filters\":{}}";
    serve_manifest(f, manifest_json(14, 1, std::string(128, 'b')));
    serve_db(f, db);

    const std::string dest = temp_dest("keep");
    { std::ofstream out(dest, std::ios::binary); out << "PREVIOUS"; }

    QEUpdater up(f, kBase, pub.data());
    auto chk = up.check(13);
    REQUIRE(up.install(chk.manifest, dest) == UpdateOutcome::DIGEST_MISMATCH);
    REQUIRE(read_file(dest) == "PREVIOUS");
}

TEST_CASE("QEUpdater: an unwritable destination reports INSTALL_FAILED", "[qe_update]") {
    FakeFetcher f;
    auto pub = unhex(kTestPub);
    const std::string db = "{\"schema_version\":1,\"cameras\":{},\"filters\":{}}";
    const std::string digest =
        sha512_hex(reinterpret_cast<const unsigned char*>(db.data()), db.size());
    serve_manifest(f, manifest_json(14, 1, digest));
    serve_db(f, db);

    QEUpdater up(f, kBase, pub.data());
    auto chk = up.check(13);
    REQUIRE(up.install(chk.manifest, "/proc/nukex_cannot_write/qe_database.json")
            == UpdateOutcome::INSTALL_FAILED);
}
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd build && make test_qe_update 2>&1 | tail -5`
Expected: `no member named 'install' in 'nukex::QEUpdater'`.

- [ ] **Step 3: Implement `install()`**

Add to the header:

```cpp
    // Fetches the database named by `manifest`, verifies its signature and
    // its digest against the manifest, then replaces dest_path atomically.
    // Writes nothing unless every check passes.
    UpdateOutcome install(const QEManifest& manifest,
                          const std::string& dest_path) const;
```

Implement in this order: fetch body + signature (failure → `OFFLINE`); verify the signature (failure → `BAD_SIGNATURE`); compare `sha512_hex(body)` against `manifest.db_sha512` case-insensitively (mismatch → `DIGEST_MISMATCH`); create the parent directory; write `dest_path + ".tmp"`, flush and `fsync` it, close, then `std::filesystem::rename` onto `dest_path`; any filesystem error → remove the temp file and return `INSTALL_FAILED`.

The temp file must sit beside the destination, not in `/tmp`, so the rename stays within one filesystem and is therefore atomic.

- [ ] **Step 4: Run to confirm the tests pass**

Run: `cd build && make test_qe_update -j$(nproc) && ./test/test_qe_update "[qe_update]"`
Expected: all 13 cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/lib/calibration test/unit/calibration/test_qe_update.cpp
git commit -m "$(cat <<'EOF'
feat(calibration): QEUpdater::install -- verified, atomic database replace

Nothing is written until the downloaded body passes both its own Ed25519
signature and the SHA-512 digest named by the already-verified manifest.
The digest is not redundant with the signature: it closes the gap where a
different, validly signed database is substituted for the one the manifest
describes.

The scratch file is written beside the destination rather than in /tmp, so
the rename stays within one filesystem and is genuinely atomic; an
interrupted install therefore cannot leave a truncated database where a
working one was. A failed install is covered by a test that asserts the
previous file is still byte-for-byte intact.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Task 4: Update state persistence

**Files:**
- Create: `src/lib/calibration/include/nukex/calibration/qe_update_state.hpp`, `src/lib/calibration/src/qe_update_state.cpp`
- Create: `test/unit/calibration/test_qe_update_state.cpp`
- Modify: `src/lib/calibration/CMakeLists.txt`, `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `UpdateOutcome` (Task 2).
- Produces: `QEUpdateState`, `load_update_state(path)`, `save_update_state(path, state)`, `should_check_now(state, now_unix)`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/calibration/qe_update_state.hpp"

#include <filesystem>
#include <fstream>

using namespace nukex;
namespace fs = std::filesystem;

static std::string state_path(const char* tag) {
    fs::path p = fs::temp_directory_path() /
                 ("nukex_qe_state_" + std::string(tag) + "_" + std::to_string(::getpid()));
    fs::create_directories(p);
    return (p / "qe_update_state.json").string();
}

TEST_CASE("update state: round-trips through JSON", "[qe_update_state]") {
    QEUpdateState s;
    s.enabled              = false;
    s.interval_days        = 14;
    s.last_check_unix      = 1757000000;
    s.installed_db_version = 14;
    s.declined_version     = 15;

    const std::string p = state_path("round");
    REQUIRE(save_update_state(p, s));

    QEUpdateState back = load_update_state(p);
    REQUIRE(back.enabled              == false);
    REQUIRE(back.interval_days        == 14);
    REQUIRE(back.last_check_unix      == 1757000000);
    REQUIRE(back.installed_db_version == 14);
    REQUIRE(back.declined_version     == 15);
}

TEST_CASE("update state: a missing file yields usable defaults", "[qe_update_state]") {
    QEUpdateState s = load_update_state(state_path("missing") + ".nope");
    REQUIRE(s.enabled);
    REQUIRE(s.interval_days == 7);
    REQUIRE(s.last_check_unix == 0);
    REQUIRE(s.installed_db_version == 0);
}

TEST_CASE("update state: a corrupt file yields defaults rather than throwing", "[qe_update_state]") {
    const std::string p = state_path("corrupt");
    { std::ofstream out(p); out << "{ this is not json"; }
    QEUpdateState s = load_update_state(p);
    REQUIRE(s.enabled);
    REQUIRE(s.interval_days == 7);
}

TEST_CASE("update state: the interval gates checking", "[qe_update_state]") {
    QEUpdateState s;
    s.enabled         = true;
    s.interval_days   = 7;
    s.last_check_unix = 1000000;

    REQUIRE_FALSE(should_check_now(s, 1000000 + 6 * 86400));
    REQUIRE      (should_check_now(s, 1000000 + 7 * 86400));
}

TEST_CASE("update state: disabling suppresses checking regardless of age", "[qe_update_state]") {
    QEUpdateState s;
    s.enabled         = false;
    s.interval_days   = 7;
    s.last_check_unix = 0;
    REQUIRE_FALSE(should_check_now(s, 99999999));
}
```

- [ ] **Step 1a: Register the test**

```cmake
nukex_add_test(test_qe_update_state unit/calibration/test_qe_update_state.cpp nukex4_calibration nukex4_io nukex4_core)
```

- [ ] **Step 2: Run to confirm failure**

Run: `cd build && cmake .. && make test_qe_update_state 2>&1 | tail -5`
Expected: `nukex/calibration/qe_update_state.hpp: No such file or directory`.

- [ ] **Step 3: Implement**

`QEUpdateState` carries `bool enabled = true; int interval_days = 7; long long last_check_unix = 0; std::string last_result; int installed_db_version = 0; int declined_version = 0;`. `load_update_state` returns a default-constructed state for any missing or unparseable file — losing this file costs one extra check and nothing more, so it must never be an error. `save_update_state` writes via the same temp-then-rename discipline as Task 3. `should_check_now` returns `state.enabled && (now_unix - last_check_unix) >= interval_days * 86400`.

Add `src/qe_update_state.cpp` to `src/lib/calibration/CMakeLists.txt`.

- [ ] **Step 4: Run to confirm the tests pass**

Run: `cd build && make test_qe_update_state -j$(nproc) && ./test/test_qe_update_state "[qe_update_state]"`
Expected: 5 cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/lib/calibration test/unit/calibration/test_qe_update_state.cpp test/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(calibration): QE update state -- interval gate and declined versions

Persists enabled/interval/last-check/installed-version/declined-version
beside the Phase 8 user state. A missing or corrupt file yields defaults
rather than an error: losing it costs one extra check and nothing else, so
failing loudly there would be noise, not signal.

declined_version is what stops the consent prompt reappearing every
interval for a version the user already said no to, without disabling
updates altogether.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Task 5: Module glue — fetcher, path precedence, provenance keyword

**Files:**
- Create: `src/module/QEFetcher.h`, `src/module/QEFetcher.cpp`
- Modify: `src/module/NukeXInstance.cpp` (near `PIShareRoot()` at line 116, `config.qe_database_path` at line 444, composed keywords at line 647)
- Modify: `src/module/CMakeLists.txt`

**Interfaces:**
- Consumes: `Fetcher` (Task 2), `load_update_state` (Task 4).
- Produces: `pcl::QEFetcher`, `nukex::QEUserDataRoot()`, `nukex::ResolveQEDatabasePath(share_root, user_root)`.

- [ ] **Step 1: Implement `QEFetcher`**

```cpp
// src/module/QEFetcher.h
#ifndef NUKEX_MODULE_QEFETCHER_H
#define NUKEX_MODULE_QEFETCHER_H

#include "nukex/calibration/qe_update.hpp"
#include <pcl/NetworkTransfer.h>

namespace pcl {

// Fetcher over PCL NetworkTransfer. Lives in the module because
// nukex4_calibration must not link PCL.
class QEFetcher : public nukex::Fetcher
{
public:
   nukex::FetchResult get( const std::string& url ) override;

private:
   String m_data;
   bool   OnDownloadDataAvailable( NetworkTransfer&, const void*, fsize_type );
};

} // namespace pcl
#endif
```

The implementation sets `SetSSL( true, true, true, true )` — TLS required, peer and host both verified — installs the download-data handler, calls `Download()`, and returns `{ false, "", <error> }` on any failure. Never throw out of `get()`: an offline observatory laptop is the normal case.

- [ ] **Step 2: Add the path-precedence helper**

In `NukeXInstance.cpp`, beside `PIShareRoot()`:

```cpp
// <user-data>/nukex4 — the same 0700 directory resolve_user_data_paths()
// creates for the Phase 8 state. Downloads land here because <PI>/share is
// root-owned in a normal install and a user must not need to write there.
static std::string QEUserDataRoot()
{
   const char* home = std::getenv( "HOME" );
   return ( home ? std::string( home ) + "/.config" : std::string( "/tmp" ) )
          + "/nukex4";
}

// Precedence: a downloaded database wins over the shipped one. The shipped
// copy is never modified, so deleting one file always restores it.
static std::string ResolveQEDatabasePath()
{
   const std::string downloaded = QEUserDataRoot() + "/qe_database.json";
   std::error_code ec;
   if ( std::filesystem::exists( downloaded, ec ) && !ec )
      return downloaded;
   return PIShareRoot() + "/qe_database.json";
}
```

Change line 444 from `config.qe_database_path = PIShareRoot() + "/qe_database.json";` to `config.qe_database_path = ResolveQEDatabasePath();`.

The engine is deliberately untouched: it still loads one shipped path then layers the user override, so `qe_overrides.json` keeps winning over everything.

- [ ] **Step 3: Record the version in the composed header**

At `NukeXInstance.cpp:647`, after the `NUKEX_QE_CONFIDENCE` keyword, append:

```cpp
      cw_ka.Append( pcl::FITSHeaderKeyword(
          "NUKEX_QE_DB_VERSION",
          pcl::IsoString().Format( "%d",
              nukex::load_update_state( QEUserDataRoot() + "/qe_update_state.json" )
                  .installed_db_version ),
          "QE camera database version used for the Phase B solve" ) );
```

A result that changes because the data changed is then explainable from the header rather than mysterious.

- [ ] **Step 4: Build and confirm the suite is green**

Run: `cd build && cmake .. && make -j$(nproc) 2>&1 | grep -E "error:" | head -5; ctest 2>&1 | tail -4`
Expected: no errors, `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/module
git commit -m "$(cat <<'EOF'
feat(module): NetworkTransfer fetcher, QE path precedence, DB version keyword

A downloaded database now takes precedence over the shipped one, resolved
in the module so the stacking engine is untouched and qe_overrides.json
keeps winning over both. Downloads land in the existing 0700 per-user
directory beside the Phase 8 state rather than root-owned <PI>/share; the
shipped copy is never modified, so removing one file restores it.

TLS is required with peer and host verification on, and get() never throws
-- an observatory laptop with no network is the normal case, not an error.

NUKEX_QE_DB_VERSION joins NUKEX_QE_CONFIDENCE on the composed window so a
stack whose colours moved because the camera data moved is explainable
from its own header.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Task 6: Interface — interval check, consent, opt-out

**Files:**
- Modify: `src/module/NukeXInterface.h`, `src/module/NukeXInterface.cpp`

**Interfaces:**
- Consumes: `QEUpdater`, `QEFetcher`, update state (Tasks 2-5).
- Produces: no new public API.

- [ ] **Step 1: Add the controls**

In the `Options_Sizer` region where the Task 13 QE override picker lives, add a `CheckBox` ("Check for camera database updates"), a `PushButton` ("Check now"), and a `Label` showing the installed version and last check. Follow the existing `e_QEOverrideBrowse` naming used in that file rather than the PCL template's double-underscore convention.

- [ ] **Step 2: Wire the interval check**

On interface open, if `should_check_now( state, time(nullptr) )`, run `QEUpdater::check()`. On `AVAILABLE` with `manifest.db_version != state.declined_version`, show a `MessageBox` naming the version, camera and sensor counts, and `summary`, and install only on acceptance. Persist `last_check_unix` on every attempt regardless of outcome, so a failing check does not retry on every open.

Never call `check()` while a stack is running.

- [ ] **Step 3: Report outcomes**

`BAD_SIGNATURE` and `DIGEST_MISMATCH` write a loud Process Console warning naming the file. `OFFLINE` is silent. `SCHEMA_TOO_NEW` warns once that the module is older than the published data. Everything else is informational.

- [ ] **Step 4: Build and confirm the suite is green**

Run: `cd build && make -j$(nproc) 2>&1 | grep -E "error:" | head -5; ctest 2>&1 | tail -4`
Expected: no errors, `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/module
git commit -m "$(cat <<'EOF'
feat(module): camera-database update check, consent prompt, opt-out

Checks at most once per interval when the interface opens, never during a
stack, and installs only after the user accepts a prompt naming the
version and what it adds. Pixels therefore never change without consent,
while staleness still surfaces without anyone remembering to look -- which
is the whole point, since a database nobody checks is the SPCC failure
mode this feature exists to avoid.

last_check_unix is persisted on every attempt including failures, so a
broken endpoint cannot turn every interface open into a network round
trip. A declined version is remembered so the prompt does not reappear
each interval without disabling updates altogether.

Signature and digest failures are loud and name the file; being offline is
silent.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Task 7: Maintainer signing tool and published artifacts

**Files:**
- Create: `tools/sign_qe_database.py`
- Create: `repository/qe_manifest.json`, `repository/qe_manifest.json.sig`, `repository/qe_database.json`, `repository/qe_database.json.sig`
- Modify: `.gitignore` (the repository tarball rule must not swallow these), `docs/qe_overrides.md`

- [ ] **Step 1: Generate the signing keypair**

```bash
python3 -c "
import nacl.signing, binascii
k = nacl.signing.SigningKey.generate()
open('/home/scarter4work/projects/keys/nukex_qe_signing.key','wb').write(bytes(k))
print('public key hex:', binascii.hexlify(bytes(k.verify_key)).decode())
"
chmod 600 /home/scarter4work/projects/keys/nukex_qe_signing.key
```

The private key lives beside the `.xssk` keys and is **never** committed. Paste the printed public key into `src/lib/calibration/src/qe_update.cpp` as `kQEPublicKey`.

- [ ] **Step 2: Write `tools/sign_qe_database.py`**

Takes `share/qe_database.json` and the private key; emits `repository/qe_database.json`, its `.sig`, a regenerated `repository/qe_manifest.json` (with `db_version` bumped, `db_sha512`, `db_bytes`, camera and sensor counts read from the database, `released_utc`, and a `--summary` argument), and the manifest's `.sig`. Signatures are base64 of the detached 64-byte Ed25519 signature.

- [ ] **Step 3: Verify round-trip against the real code**

```bash
python3 tools/sign_qe_database.py share/qe_database.json \
    --key /home/scarter4work/projects/keys/nukex_qe_signing.key \
    --db-version 1 --summary "Initial published camera database." \
    --out repository/
cd build && ./test/test_qe_update "[qe_update]"
```

Then confirm the shipped artifacts verify with the module's own verifier rather than only with Python — a Python-only check would not catch a base64 or byte-order disagreement between the two implementations.

- [ ] **Step 4: Commit**

```bash
git add tools/sign_qe_database.py repository .gitignore docs/qe_overrides.md
git commit -m "$(cat <<'EOF'
feat(tools): sign and publish the QE camera database

sign_qe_database.py produces the four published artifacts from
share/qe_database.json: the database, a regenerated manifest carrying
db_version, SHA-512, byte count and camera/sensor counts, and a detached
Ed25519 signature over each. The private key lives beside the .xssk keys
and is never committed.

The round-trip is checked with the module's own verifier, not only with
Python: a base64 or byte-order disagreement between the signing tool and
the C++ verifier would otherwise ship undetected and reject every update
in the field.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01FBQ4YTa1egBbm5761T1v3E
EOF
)"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3 vendored Ed25519, no PCL crypto | 1 |
| §4 published artifacts, signed manifest + database | 2, 3, 7 |
| §5 `QEUpdater`, injected `Fetcher`, `check`/`install` split | 2, 3 |
| §6 storage, precedence, `NUKEX_QE_DB_VERSION` | 5 |
| §7 failure handling matrix | 2, 3, 6 |
| §8 update state | 4 |
| §9 testing | 1, 2, 3, 4 |
| §10 out of scope | not implemented, by design |

**Type consistency:** `Fetcher::get`, `FetchResult{ok,body,error}`, `UpdateOutcome`, `QEManifest`, `CheckResult{outcome,manifest,detail}`, `QEUpdateState`, `should_check_now`, `load_update_state`, `save_update_state` are declared in Tasks 2 and 4 and used with identical names and signatures in Tasks 3, 5 and 6. `db_sha512` is used consistently; there is no remaining `db_sha256`.

**Known placeholder, deliberate:** `kTestPub` / `kTestSec` in Task 2 must be filled by running the generator command given in that step. They are test-only key material that cannot be committed blind — the command that produces them is in the step, so no information is missing.

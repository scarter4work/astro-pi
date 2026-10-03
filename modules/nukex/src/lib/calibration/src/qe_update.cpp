#include "nukex/calibration/qe_update.hpp"
#include "nukex/calibration/ed25519_verify.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <utility>
#include <vector>

namespace nukex {

namespace {

// Strict base64 decode. Returns false on any character outside the alphabet,
// so a truncated or HTML error page served in place of a signature is
// rejected here rather than becoming a confusing verification failure.
bool base64_decode(const std::string& in, std::vector<unsigned char>& out) {
    auto value = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    out.clear();
    int  accum = 0, bits = 0;
    bool padding_seen = false;

    for (unsigned char c : in) {
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        if (c == '=') { padding_seen = true; continue; }
        if (padding_seen) return false;          // data after padding
        const int v = value(c);
        if (v < 0) return false;
        accum = (accum << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accum >> bits) & 0xFF));
        }
    }
    return true;
}

bool verify_body(const std::string& body, const std::string& signature_b64,
                 const unsigned char* public_key) {
    std::vector<unsigned char> sig;
    if (!base64_decode(signature_b64, sig)) return false;
    return ed25519_verify_detached(
        sig.data(), sig.size(),
        reinterpret_cast<const unsigned char*>(body.data()), body.size(),
        public_key);
}

} // namespace

// Ed25519 public key for NukeX camera-database publications. Rotating this
// requires shipping a new module build, which is the point: the key is the
// root of trust and must not itself be updatable over the network.
const unsigned char* qe_signing_public_key() {
    static const unsigned char kQEPublicKey[kEd25519PublicKeyBytes] = {
        0xde, 0x00, 0x3c, 0xe9, 0x28, 0x24, 0xc4, 0x7f,
        0xd8, 0x2d, 0x12, 0xa1, 0x7f, 0x5a, 0xd4, 0x11,
        0x2a, 0x0a, 0xc3, 0x76, 0x03, 0x4c, 0xcc, 0x9b,
        0xb4, 0x77, 0x8f, 0x0a, 0x51, 0xa5, 0x10, 0xcc
    };
    return kQEPublicKey;
}

const char* to_string(UpdateOutcome o) {
    switch (o) {
        case UpdateOutcome::UP_TO_DATE:         return "up to date";
        case UpdateOutcome::AVAILABLE:          return "update available";
        case UpdateOutcome::INSTALLED:          return "installed";
        case UpdateOutcome::OFFLINE:            return "offline";
        case UpdateOutcome::BAD_SIGNATURE:      return "signature verification failed";
        case UpdateOutcome::SCHEMA_TOO_NEW:     return "published schema is newer than this build";
        case UpdateOutcome::REFUSED_ROLLBACK:   return "refused: published version is older than installed";
        case UpdateOutcome::DIGEST_MISMATCH:    return "digest mismatch";
        case UpdateOutcome::MALFORMED_MANIFEST: return "malformed manifest";
        case UpdateOutcome::INSTALL_FAILED:     return "install failed";
    }
    return "unknown";
}

QEUpdater::QEUpdater(Fetcher& fetcher, std::string base_url,
                     const unsigned char* public_key)
    : fetcher_(fetcher), base_url_(std::move(base_url)), public_key_(public_key) {}

CheckResult QEUpdater::check(int installed_db_version) const {
    CheckResult result;

    const std::string url = base_url_ + "/qe_manifest.json";
    const FetchResult body = fetcher_.get(url);
    if (!body.ok) {
        result.outcome = UpdateOutcome::OFFLINE;
        result.detail  = body.error;
        return result;
    }
    const FetchResult sig = fetcher_.get(url + ".sig");
    if (!sig.ok) {
        result.outcome = UpdateOutcome::OFFLINE;
        result.detail  = sig.error;
        return result;
    }

    // Verify BEFORE parsing: unauthenticated bytes must never reach the JSON
    // parser, and a forged version number must never be read even once.
    if (!verify_body(body.body, sig.body, public_key_)) {
        result.outcome = UpdateOutcome::BAD_SIGNATURE;
        result.detail  = "manifest signature did not verify against the shipped public key";
        return result;
    }

    QEManifest m;
    try {
        const auto doc = nlohmann::json::parse(body.body);
        m.schema_version = doc.at("schema_version").get<int>();
        m.db_version     = doc.at("db_version").get<int>();
        m.db_sha512      = doc.at("db_sha512").get<std::string>();
        m.db_bytes       = doc.value("db_bytes", 0LL);
        m.n_cameras      = doc.value("n_cameras", 0);
        m.n_sensors      = doc.value("n_sensors", 0);
        m.released_utc   = doc.value("released_utc", std::string{});
        m.summary        = doc.value("summary", std::string{});
    } catch (const std::exception& e) {
        result.outcome = UpdateOutcome::MALFORMED_MANIFEST;
        result.detail  = e.what();
        return result;
    }

    if (m.schema_version > kSupportedSchemaVersion) {
        result.outcome  = UpdateOutcome::SCHEMA_TOO_NEW;
        result.manifest = m;
        result.detail   = "published schema " + std::to_string(m.schema_version) +
                          " exceeds supported " + std::to_string(kSupportedSchemaVersion);
        return result;
    }

    // An older published version is refused rather than ignored: silently
    // accepting it is a rollback attack, and reporting it as "up to date"
    // would hide a mis-publication.
    if (m.db_version < installed_db_version) {
        result.outcome  = UpdateOutcome::REFUSED_ROLLBACK;
        result.manifest = m;
        result.detail   = "published " + std::to_string(m.db_version) +
                          " is older than installed " + std::to_string(installed_db_version);
        return result;
    }
    if (m.db_version == installed_db_version) {
        result.outcome  = UpdateOutcome::UP_TO_DATE;
        result.manifest = m;
        return result;
    }

    result.outcome  = UpdateOutcome::AVAILABLE;
    result.manifest = m;
    return result;
}

UpdateOutcome QEUpdater::install(const QEManifest& manifest,
                                 const std::string& dest_path) const {
    namespace fs = std::filesystem;

    const std::string url = base_url_ + "/qe_database.json";
    const FetchResult body = fetcher_.get(url);
    if (!body.ok) return UpdateOutcome::OFFLINE;
    const FetchResult sig = fetcher_.get(url + ".sig");
    if (!sig.ok) return UpdateOutcome::OFFLINE;

    if (!verify_body(body.body, sig.body, public_key_))
        return UpdateOutcome::BAD_SIGNATURE;

    // Not redundant with the signature: this is what stops a different but
    // validly signed database being substituted for the one the (already
    // verified) manifest describes.
    const std::string got = sha512_hex(
        reinterpret_cast<const unsigned char*>(body.body.data()), body.body.size());
    std::string want = manifest.db_sha512;
    std::transform(want.begin(), want.end(), want.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (got != want) return UpdateOutcome::DIGEST_MISMATCH;

    std::error_code ec;
    const fs::path dest(dest_path);
    if (dest.has_parent_path()) {
        fs::create_directories(dest.parent_path(), ec);
        if (ec) return UpdateOutcome::INSTALL_FAILED;
    }

    // The scratch file sits beside the destination, never in /tmp, so the
    // rename stays within one filesystem and is therefore atomic. An
    // interrupted install cannot leave a truncated database behind.
    const fs::path tmp = dest_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return UpdateOutcome::INSTALL_FAILED;
        out.write(body.body.data(), static_cast<std::streamsize>(body.body.size()));
        out.flush();
        if (!out) { fs::remove(tmp, ec); return UpdateOutcome::INSTALL_FAILED; }
    }

    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return UpdateOutcome::INSTALL_FAILED;
    }
    return UpdateOutcome::INSTALLED;
}

} // namespace nukex

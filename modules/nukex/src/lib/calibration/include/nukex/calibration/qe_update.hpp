#ifndef NUKEX_CALIBRATION_QE_UPDATE_HPP
#define NUKEX_CALIBRATION_QE_UPDATE_HPP

#include <string>

namespace nukex {

struct FetchResult {
    bool        ok = false;
    std::string body;
    std::string error;
};

// Injected, so nukex4_calibration never links PCL and no test touches the
// network. The module supplies a NetworkTransfer-backed implementation.
class Fetcher {
public:
    virtual ~Fetcher() = default;
    virtual FetchResult get(const std::string& url) = 0;
};

enum class UpdateOutcome {
    UP_TO_DATE,
    AVAILABLE,           // newer version, verified; nothing written yet
    INSTALLED,
    OFFLINE,             // silent; retry next interval
    BAD_SIGNATURE,       // loud: possible tampering
    SCHEMA_TOO_NEW,      // published data is newer than this build understands
    REFUSED_ROLLBACK,
    DIGEST_MISMATCH,
    MALFORMED_MANIFEST,
    INSTALL_FAILED
};

const char* to_string(UpdateOutcome o);

// The shipped signing public key (kEd25519PublicKeyBytes bytes). Published
// data is signed with the matching private key, which lives outside this
// repository beside the .xssk keys and is never committed.
const unsigned char* qe_signing_public_key();

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
    QEManifest    manifest;   // populated only when outcome == AVAILABLE
    std::string   detail;     // human-readable, for the Process Console
};

class QEUpdater {
public:
    // base_url carries no trailing slash; file names are appended.
    // public_key must point at kEd25519PublicKeyBytes bytes outliving this.
    QEUpdater(Fetcher& fetcher, std::string base_url, const unsigned char* public_key);

    // Highest manifest/database schema this build can consume.
    static constexpr int kSupportedSchemaVersion = 1;

    // Fetches and verifies the manifest, then compares db_version against
    // installed_db_version. Writes nothing, and never fetches the database.
    CheckResult check(int installed_db_version) const;

    // Fetches the database named by `manifest`, verifies its signature AND
    // its digest against the manifest, then replaces dest_path atomically.
    // Writes nothing at all unless every check passes, so a failure always
    // leaves whatever was already installed byte-for-byte intact.
    UpdateOutcome install(const QEManifest& manifest,
                          const std::string& dest_path) const;

private:
    Fetcher&             fetcher_;
    std::string          base_url_;
    const unsigned char* public_key_;
};

} // namespace nukex

#endif

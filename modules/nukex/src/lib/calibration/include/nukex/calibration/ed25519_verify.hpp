#ifndef NUKEX_CALIBRATION_ED25519_VERIFY_HPP
#define NUKEX_CALIBRATION_ED25519_VERIFY_HPP

#include <cstddef>
#include <string>

namespace nukex {

// Ed25519 public keys are 32 bytes; detached signatures are 64.
inline constexpr std::size_t kEd25519PublicKeyBytes = 32;
inline constexpr std::size_t kEd25519SignatureBytes = 64;

// Verifies a DETACHED Ed25519 signature. TweetNaCl offers only the combined
// crypto_sign_open(), where the signature is prepended to the message, so
// this builds "sig || msg" in a scratch buffer and calls that. Returns false
// -- never throws, never aborts -- for any bad input, a null pointer and a
// wrong-length signature included.
bool ed25519_verify_detached(const unsigned char* sig, std::size_t sig_len,
                             const unsigned char* msg, std::size_t msg_len,
                             const unsigned char* public_key);

// Lowercase hex SHA-512, via TweetNaCl's crypto_hash. Used for the update
// manifest's db_sha512. SHA-512 rather than SHA-256 because this library must
// not link PCL (so pcl::SHA256 is unreachable) and TweetNaCl already carries
// SHA-512 -- a second hash dependency would buy nothing.
std::string sha512_hex(const unsigned char* data, std::size_t len);

} // namespace nukex

#endif

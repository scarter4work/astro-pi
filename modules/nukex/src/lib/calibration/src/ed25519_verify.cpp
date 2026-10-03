#include "nukex/calibration/ed25519_verify.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
#include "tweetnacl.h"

// TweetNaCl declares randombytes() extern for KEY GENERATION and will not
// link without a definition. Verification never calls it. Supplying weak
// randomness merely to satisfy the linker would sit here silently until
// somebody added signing and got predictable keys, so fail loudly instead.
void randombytes(unsigned char*, unsigned long long) {
    std::fprintf(stderr,
                 "nukex: randombytes() called -- this build vendors TweetNaCl for "
                 "verification only and has no CSPRNG wired up.\n");
    std::abort();
}
}

namespace nukex {

bool ed25519_verify_detached(const unsigned char* sig, std::size_t sig_len,
                             const unsigned char* msg, std::size_t msg_len,
                             const unsigned char* public_key) {
    if (sig == nullptr || public_key == nullptr) return false;
    if (sig_len != kEd25519SignatureBytes)       return false;
    if (msg == nullptr && msg_len != 0)          return false;

    // crypto_sign_open() consumes "signature || message" and writes back a
    // plaintext no longer than its input.
    std::vector<unsigned char> signed_message(sig_len + msg_len);
    std::copy(sig, sig + sig_len, signed_message.begin());
    if (msg_len > 0)
        std::copy(msg, msg + msg_len, signed_message.begin() + sig_len);

    std::vector<unsigned char> plain(signed_message.size());
    unsigned long long plain_len = 0;

    return crypto_sign_open(plain.data(), &plain_len,
                            signed_message.data(),
                            static_cast<unsigned long long>(signed_message.size()),
                            public_key) == 0;
}

std::string sha512_hex(const unsigned char* data, std::size_t len) {
    unsigned char digest[crypto_hash_BYTES];
    // crypto_hash tolerates len == 0; data may be any valid pointer then.
    static const unsigned char kEmpty[1] = { 0 };
    crypto_hash(digest, (len == 0 ? kEmpty : data),
                static_cast<unsigned long long>(len));

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

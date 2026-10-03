#include "catch_amalgamated.hpp"
#include "nukex/calibration/ed25519_verify.hpp"

#include <string>
#include <vector>

using namespace nukex;

static std::vector<unsigned char> unhex(const std::string& h) {
    std::vector<unsigned char> out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2)
        out.push_back(static_cast<unsigned char>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}

// RFC 8032 section 7.1, TEST 2 -- a one-byte message. Using published
// vectors is the point: a verifier that returns true unconditionally, or a
// mis-vendored bignum, passes a self-signed round-trip but fails these.
static const char* kPub =
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c";
static const char* kSig =
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
    "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00";
static const unsigned char kMsg[1] = { 0x72 };

TEST_CASE("ed25519: a genuine RFC 8032 signature verifies", "[ed25519]") {
    auto pub = unhex(kPub);
    auto sig = unhex(kSig);
    REQUIRE(ed25519_verify_detached(sig.data(), sig.size(), kMsg, sizeof(kMsg), pub.data()));
}

TEST_CASE("ed25519: a flipped message byte fails", "[ed25519]") {
    auto pub = unhex(kPub);
    auto sig = unhex(kSig);
    const unsigned char bad[1] = { 0x73 };
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), sig.size(), bad, sizeof(bad), pub.data()));
}

TEST_CASE("ed25519: a flipped signature byte fails", "[ed25519]") {
    auto pub = unhex(kPub);
    auto sig = unhex(kSig);
    sig[0] ^= 0x01;
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), sig.size(), kMsg, sizeof(kMsg), pub.data()));
}

TEST_CASE("ed25519: a wrong-length signature is rejected", "[ed25519]") {
    auto pub = unhex(kPub);
    auto sig = unhex(kSig);
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), 63, kMsg, sizeof(kMsg), pub.data()));
}

TEST_CASE("ed25519: a null signature or key is rejected, not dereferenced", "[ed25519]") {
    auto pub = unhex(kPub);
    auto sig = unhex(kSig);
    REQUIRE_FALSE(ed25519_verify_detached(nullptr, kEd25519SignatureBytes,
                                          kMsg, sizeof(kMsg), pub.data()));
    REQUIRE_FALSE(ed25519_verify_detached(sig.data(), sig.size(),
                                          kMsg, sizeof(kMsg), nullptr));
}

TEST_CASE("sha512_hex matches the published empty-string digest", "[ed25519]") {
    REQUIRE(sha512_hex(reinterpret_cast<const unsigned char*>(""), 0) ==
            "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
            "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
}

TEST_CASE("sha512_hex matches the published digest for \"abc\"", "[ed25519]") {
    const std::string m = "abc";
    REQUIRE(sha512_hex(reinterpret_cast<const unsigned char*>(m.data()), m.size()) ==
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
            "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
}

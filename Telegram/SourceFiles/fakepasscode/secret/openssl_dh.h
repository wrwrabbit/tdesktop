#pragma once

#include "base/openssl_help.h"
#include "base/bytes.h"
#include <vector>
#include <memory>

namespace openssl {

class DHKey {
public:
    // Generate a new DH key pair (private and public) with custom p/g
    static std::unique_ptr<DHKey> Generate(bytes::const_span p, int32_t g);

    // Construct from existing private value and custom p/g
    DHKey(const openssl::BigNum &privateValue, const openssl::BigNum &p, const openssl::BigNum &g);

    // Get the public key (g^a mod p)
    bytes::vector publicKey() const;

    // Get the private key (a)
    bytes::vector privateKey() const;

    // Compute shared secret (g^b)^a mod p
    bytes::vector computeSharedSecret(bytes::const_span peerPublicKey) const;

private:
    openssl::BigNum _private;
    openssl::BigNum _public;
    openssl::BigNum _p;
    openssl::BigNum _g;

    void generate();
};

} // namespace openssl

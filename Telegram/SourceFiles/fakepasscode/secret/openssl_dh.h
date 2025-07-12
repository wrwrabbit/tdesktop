#pragma once

#include "base/openssl_help.h"
#include "base/bytes.h"
#include <vector>
#include <memory>

namespace openssl {

class DHKey {
public:
    // Standard Telegram DH params (2048 bits, see https://core.telegram.org/api/end-to-end)
    static const bytes::const_span kDefaultG();
    static const bytes::const_span kDefaultP();

    // Generate a new DH key pair (private and public)
    static std::unique_ptr<DHKey> Generate();

    // Construct from existing private value
    explicit DHKey(const openssl::BigNum &privateValue);

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

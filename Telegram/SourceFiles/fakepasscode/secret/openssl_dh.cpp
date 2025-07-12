#include "fakepasscode/secret/openssl_dh.h"
#include "base/openssl_help.h"
#include "base/random.h"
#include "base/bytes.h"
extern "C" {
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
}
#include <cassert>

namespace openssl {

// Telegram's default DH parameters (2048-bit MODP Group, see https://core.telegram.org/api/end-to-end)

std::unique_ptr<DHKey> DHKey::Generate(bytes::const_span p, int32_t g) {
    // Generate a random private key (256 bytes)
    bytes::vector priv(256);
    base::RandomFill(priv);
    auto privBN = openssl::BigNum();
    privBN.setBytes(priv);

    auto pBN = openssl::BigNum();
    pBN.setBytes(p);

    auto gBN = openssl::BigNum();
    gBN.setWord(g);

    auto key = std::make_unique<DHKey>(privBN, pBN, gBN);
    key->generate();
    return key;
}

DHKey::DHKey(const openssl::BigNum &privateValue, const openssl::BigNum& p, const openssl::BigNum& g)
    : _private(privateValue)
    , _p(p)
    , _g(g) {
    generate();
}

void DHKey::generate() {
    // Compute public key: g^a mod p
    _public = openssl::BigNum::ModExp(_g, _private, _p);
}

bytes::vector DHKey::publicKey() const {
    return _public.getBytes();
}

bytes::vector DHKey::privateKey() const {
    return _private.getBytes();
}

bytes::vector DHKey::computeSharedSecret(bytes::const_span peerPublicKey) const {
    auto peerPub = openssl::BigNum();
    peerPub.setBytes(peerPublicKey);
    auto shared = openssl::BigNum::ModExp(peerPub, _private, _p);
    return shared.getBytes();
}

} // namespace openssl
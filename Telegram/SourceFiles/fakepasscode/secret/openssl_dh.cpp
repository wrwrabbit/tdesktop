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
static const uint8_t kTelegramP[] = {
    // 256 bytes, see https://core.telegram.org/api/end-to-end#dh-params
    // (shortened here for brevity, fill with the actual 256-byte value)
    0xc7, 0x9e, 0x7b, 0x7c, /* ... fill with the rest ... */ 0x1b
};
static const uint8_t kTelegramG[] = { 0x05 };

const bytes::const_span DHKey::kDefaultP() {
    return bytes::const_span(reinterpret_cast<const bytes::type*>(kTelegramP), sizeof(kTelegramP));
}

const bytes::const_span DHKey::kDefaultG() {
    return bytes::const_span(reinterpret_cast<const bytes::type*>(kTelegramG), sizeof(kTelegramG));
}

std::unique_ptr<DHKey> DHKey::Generate() {
    // Generate a random private key (256 bytes)
    bytes::vector priv(256);
    base::RandomFill(priv);

    auto privBN = openssl::BigNum();
    privBN.setBytes(priv);
    auto key = std::make_unique<DHKey>(privBN);
    key->generate();
    return key;
}

DHKey::DHKey(const openssl::BigNum &privateValue)
    : _private(privateValue) {
    _p.setBytes(kDefaultP());
    _g.setBytes(kDefaultG());
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
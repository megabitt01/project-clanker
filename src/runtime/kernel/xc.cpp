// Crypto exports. SHA-1 / HMAC-SHA1 / RC4 are real (save-game signatures and
// content hashing rely on them); the RSA/DES entry points are only used by
// Xbox Live and content verification and are stubbed.
#include "kexports.h"
#include "../log.h"

#include <cstring>

namespace {

struct Sha1Ctx { // fits in the 0x74-byte A_SHA_CTX the title allocates
    uint32_t state[5];
    uint32_t count_lo, count_hi;
    uint8_t buffer[64];
};
static_assert(sizeof(Sha1Ctx) <= 0x74);

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_block(uint32_t st[5], const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

void sha1_init(Sha1Ctx *c)
{
    c->state[0] = 0x67452301; c->state[1] = 0xEFCDAB89; c->state[2] = 0x98BADCFE;
    c->state[3] = 0x10325476; c->state[4] = 0xC3D2E1F0;
    c->count_lo = c->count_hi = 0;
}

void sha1_update(Sha1Ctx *c, const uint8_t *data, uint32_t len)
{
    uint32_t used = c->count_lo & 63;
    uint32_t old = c->count_lo;
    c->count_lo += len;
    if (c->count_lo < old) c->count_hi++;
    while (len) {
        uint32_t n = 64 - used < len ? 64 - used : len;
        memcpy(c->buffer + used, data, n);
        used += n; data += n; len -= n;
        if (used == 64) { sha1_block(c->state, c->buffer); used = 0; }
    }
}

void sha1_final(Sha1Ctx *c, uint8_t out[20])
{
    uint64_t bits = ((uint64_t(c->count_hi) << 32) | c->count_lo) * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    uint8_t zero = 0;
    while ((c->count_lo & 63) != 56) sha1_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = uint8_t(bits >> (56 - 8 * i));
    sha1_update(c, len, 8);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = uint8_t(c->state[i] >> 24); out[i * 4 + 1] = uint8_t(c->state[i] >> 16);
        out[i * 4 + 2] = uint8_t(c->state[i] >> 8); out[i * 4 + 3] = uint8_t(c->state[i]);
    }
}

} // namespace

KEXPORT(void) kx_XcSHAInit(uint8_t *pbSHAContext) { sha1_init(reinterpret_cast<Sha1Ctx *>(pbSHAContext)); }
KEXPORT(void) kx_XcSHAUpdate(uint8_t *pbSHAContext, const uint8_t *pbInput, uint32_t dwInputLength)
{
    sha1_update(reinterpret_cast<Sha1Ctx *>(pbSHAContext), pbInput, dwInputLength);
}
KEXPORT(void) kx_XcSHAFinal(uint8_t *pbSHAContext, uint8_t *pbDigest)
{
    sha1_final(reinterpret_cast<Sha1Ctx *>(pbSHAContext), pbDigest);
}

KEXPORT(void) kx_XcHMAC(const uint8_t *pbKeyMaterial, uint32_t cbKeyMaterial, const uint8_t *pbData, uint32_t cbData,
                        const uint8_t *pbData2, uint32_t cbData2, uint8_t *HmacData)
{
    uint8_t key[64] = {};
    if (cbKeyMaterial > 64) {
        Sha1Ctx c;
        sha1_init(&c);
        sha1_update(&c, pbKeyMaterial, cbKeyMaterial);
        sha1_final(&c, key);
    } else {
        memcpy(key, pbKeyMaterial, cbKeyMaterial);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = key[i] ^ 0x36; opad[i] = key[i] ^ 0x5C; }
    uint8_t inner[20];
    Sha1Ctx c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    if (pbData) sha1_update(&c, pbData, cbData);
    if (pbData2) sha1_update(&c, pbData2, cbData2);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, HmacData);
}

struct Rc4Key { uint8_t s[256]; uint8_t i, j; };

KEXPORT(void) kx_XcRC4Key(uint8_t *pbKeyStruct, uint32_t dwKeyLength, const uint8_t *pbKey)
{
    Rc4Key *k = reinterpret_cast<Rc4Key *>(pbKeyStruct);
    for (int i = 0; i < 256; i++) k->s[i] = uint8_t(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = uint8_t(j + k->s[i] + pbKey[i % dwKeyLength]);
        uint8_t t = k->s[i]; k->s[i] = k->s[j]; k->s[j] = t;
    }
    k->i = k->j = 0;
}

KEXPORT(void) kx_XcRC4Crypt(uint8_t *pbKeyStruct, uint32_t dwInputLength, uint8_t *pbInput)
{
    Rc4Key *k = reinterpret_cast<Rc4Key *>(pbKeyStruct);
    uint8_t i = k->i, j = k->j;
    for (uint32_t n = 0; n < dwInputLength; n++) {
        i++;
        j = uint8_t(j + k->s[i]);
        uint8_t t = k->s[i]; k->s[i] = k->s[j]; k->s[j] = t;
        pbInput[n] ^= k->s[uint8_t(k->s[i] + k->s[j])];
    }
    k->i = i;
    k->j = j;
}

KEXPORT(void) kx_XcDESKeyParity(uint8_t *pbKey, uint32_t dwKeyLength)
{
    for (uint32_t i = 0; i < dwKeyLength; i++) {
        uint8_t b = pbKey[i] & 0xFE, p = b;
        p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
        pbKey[i] = b | ((p & 1) ^ 1);
    }
}

KEXPORT(void) kx_XcKeyTable(uint32_t dwCipher, uint8_t *pbKeyTable, const uint8_t *pbKey)
{
    LOG_WARN("xc", "XcKeyTable (DES) not implemented");
}

KEXPORT(void) kx_XcBlockCryptCBC(uint32_t dwCipher, uint32_t dwInputLength, uint8_t *pbOutput, const uint8_t *pbInput,
                                 const uint8_t *pbKeyTable, uint32_t dwOp, uint8_t *pbFeedback)
{
    LOG_WARN("xc", "XcBlockCryptCBC (DES) not implemented, copying plaintext");
    memmove(pbOutput, pbInput, dwInputLength);
}

KEXPORT(uint32_t) kx_XcModExp(uint32_t *pA, const uint32_t *pB, const uint32_t *pC, const uint32_t *pD, uint32_t dwN)
{
    LOG_WARN("xc", "XcModExp not implemented");
    return 0;
}

KEXPORT(XBOOLEAN) kx_XcVerifyPKCS1Signature(const uint8_t *pbSig, const uint8_t *pbPubKey, const uint8_t *pbDigest)
{
    return 1;
}

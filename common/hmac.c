#include "hmac.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
/* ---------- macOS / BSD：CommonCrypto（系统自带，无需链接库） ---------- */
#include <CommonCrypto/CommonHMAC.h>

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[HMAC_SHA256_LEN])
{
    CCHmac(kCCHmacAlgSHA256, key, key_len, data, data_len, out);
}

#else
/* ---------- Linux：OpenSSL（需链接 -lcrypto） ---------- */
#include <openssl/hmac.h>

void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[HMAC_SHA256_LEN])
{
    unsigned int md_len = 0;
    HMAC(EVP_sha256(), key, (int)key_len,
         (const unsigned char *)data, data_len,
         out, &md_len);
}

#endif

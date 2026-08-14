#ifndef HMAC_H
#define HMAC_H

#include <stddef.h>
#include <stdint.h>

#define HMAC_SHA256_LEN 32   /* HMAC-SHA256 输出长度（字节） */

/*
 * 计算 HMAC-SHA256。
 *   key      共享密钥
 *   key_len  密钥长度
 *   data     待认证数据（可由多段拼接，调用方可自行拼接后传入）
 *   data_len 数据长度
 *   out      输出缓冲区，至少 HMAC_SHA256_LEN 字节
 *
 * 跨平台：macOS 用 CommonCrypto，Linux 用 OpenSSL。
 */
void hmac_sha256(const void *key, size_t key_len,
                 const void *data, size_t data_len,
                 uint8_t out[HMAC_SHA256_LEN]);

#endif /* HMAC_H */

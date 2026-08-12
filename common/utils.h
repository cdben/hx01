#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

#include "protocol.h"

/* 循环 read 直到读满 len 字节，返回 0 成功，-1 失败。 */
int read_full(int fd, void *buf, size_t len);

/* 循环 write 直到写满 len 字节，返回 0 成功，-1 失败。 */
int write_full(int fd, const void *buf, size_t len);

/* 先读 header，校验，再读 payload，返回 0 成功，-1 失败。
 * payload 指向 buf 中 payload 起始位置（不拷贝），payload_len 写出 payload 长度。 */
int recv_message(int fd, uint8_t *buf, size_t bufsize,
                 msg_type_t *type, uint32_t *req_id,
                 uint8_t *payload, uint32_t *payload_len);

/* 打包并发送消息，返回 0 成功，-1 失败。 */
int send_message(int fd, msg_type_t type, uint32_t req_id,
                 const void *payload, uint32_t length);

/* 设置 fd 为非阻塞，返回 0 成功，-1 失败。 */
int set_nonblock(int fd);

#endif /* UTILS_H */

#include "utils.h"

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;

    if (fd < 0) {
        return -1;
    }
    if (len > 0 && buf == NULL) {
        return -1;
    }

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            /* 对端关闭，未读满。 */
            return -1;
        }
        got += (size_t)n;
    }

    return 0;
}

int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    if (fd < 0) {
        return -1;
    }
    if (len > 0 && buf == NULL) {
        return -1;
    }

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            /* 不应发生，保守返回失败。 */
            return -1;
        }
        sent += (size_t)n;
    }

    return 0;
}

int recv_message(int fd, uint8_t *buf, size_t bufsize,
                 msg_type_t *type, uint32_t *req_id,
                 uint8_t *payload, uint32_t *payload_len)
{
    msg_header_t hdr;
    uint32_t total;

    if (fd < 0 || buf == NULL || type == NULL || req_id == NULL ||
        payload_len == NULL) {
        return -1;
    }

    /* buf 至少要能容纳 header。 */
    if (bufsize < HEADER_SIZE) {
        return -1;
    }

    /* 先读 header。 */
    if (read_full(fd, buf, HEADER_SIZE) != 0) {
        return -1;
    }

    if (msg_parse_header(buf, HEADER_SIZE, &hdr) != 0) {
        return -1;
    }

    total = (uint32_t)HEADER_SIZE + hdr.length;
    if (bufsize < total) {
        /* buf 不足以容纳完整 payload。 */
        return -1;
    }

    /* 再读 payload。 */
    if (hdr.length > 0) {
        if (read_full(fd, buf + HEADER_SIZE, hdr.length) != 0) {
            return -1;
        }
    }

    *type = (msg_type_t)hdr.type;
    *req_id = hdr.req_id;
    *payload_len = hdr.length;

    /* 若调用方提供独立的 payload 缓冲区，则将 payload 从 buf 拷贝过去。
     * 调用方需确保该缓冲区容量 >= hdr.length。payload_len 写出实际长度。
     * payload 为 NULL 时跳过拷贝，调用方可直接使用 buf + HEADER_SIZE。 */
    if (payload != NULL) {
        memcpy(payload, buf + HEADER_SIZE, hdr.length);
    }

    return 0;
}

int send_message(int fd, msg_type_t type, uint32_t req_id,
                 const void *payload, uint32_t length)
{
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];

    if (fd < 0) {
        return -1;
    }
    if (length > MAX_PAYLOAD) {
        return -1;
    }

    if (msg_pack(buf, sizeof(buf), type, req_id, payload, length) < 0) {
        return -1;
    }

    if (write_full(fd, buf, (size_t)HEADER_SIZE + length) != 0) {
        return -1;
    }

    return 0;
}

int set_nonblock(int fd)
{
    int flags;

    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    return 0;
}

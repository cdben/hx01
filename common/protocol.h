#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define MAGIC 0x5A53
#define HEADER_SIZE 12
#define MAX_PAYLOAD 65536
#define MAX_CLIENTS 256
#define MAX_CLIENT_ID 64
#define MAX_CMD_LEN 4096
#define MAX_RESULT_LEN 65536

typedef enum {
    MSG_REGISTER = 1,
    MSG_REGISTER_ACK = 2,
    MSG_CMD = 3,
    MSG_RESULT = 4,
    MSG_HEARTBEAT = 5,
    MSG_HEARTBEAT_ACK = 6,
    MSG_ERROR = 7,
    MSG_LIST = 8,
    MSG_LIST_RESP = 9,
    MSG_CANCEL = 10
} msg_type_t;

typedef struct {
    uint16_t magic;
    uint8_t type;
    uint8_t reserved;
    uint32_t req_id;
    uint32_t length;  /* payload length */
} __attribute__((packed)) msg_header_t;

/* 打包消息到 buf，返回总字节数（含 header）。失败返回 -1。 */
int msg_pack(uint8_t *buf, size_t bufsize, msg_type_t type, uint32_t req_id,
             const void *payload, uint32_t length);

/* 从 buf 解析 header。成功返回 0，失败返回 -1。 */
int msg_parse_header(const uint8_t *buf, size_t buflen, msg_header_t *hdr);

/* 辅助函数：返回 type 对应的字符串名。 */
const char *msg_type_str(msg_type_t type);

#endif /* PROTOCOL_H */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define MAGIC 0x5A53
#define HEADER_SIZE 12
#define MAX_PAYLOAD 65536
#define MAX_CLIENTS 65536
#define MAX_CLIENT_ID 64
#define MAX_CMD_LEN 4096
#define MAX_RESULT_LEN 65536

/* 文件传输相关 */
#define FILE_CHUNK_SIZE (MAX_PAYLOAD - 1024)  /* 单块数据量，预留路径/头部空间 */
#define FILE_META_SIZE  9                     /* offset(4) + total(4) + flags(1) */
#define FILE_FLAG_FINAL 0x01                  /* flags 位0：最后一块 */

/* 身份认证（HMAC 挑战应答） */
#define CHALLENGE_LEN  32                     /* 认证挑战随机数长度（字节） */

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
    MSG_CANCEL = 10,
    MSG_FILE_UPLOAD = 11,
    MSG_FILE_DOWNLOAD = 12,
    MSG_FILE_DATA = 13,
    MSG_FILE_ACK = 14,
    /* 身份认证（HMAC 挑战应答） */
    MSG_AUTH_INIT = 15,     /* client/admin → server：发起认证（payload: 角色+可选id） */
    MSG_AUTH_CHALLENGE = 16,/* server → client/admin：下发随机挑战（payload: 32B 随机数） */
    MSG_AUTH_RESPONSE = 17, /* client/admin → server：应答（payload: 32B HMAC） */
    MSG_AUTH_OK = 18,       /* server → client/admin：认证通过 */
    MSG_AUTH_FAIL = 19      /* server → client/admin：认证失败 */
} msg_type_t;

typedef struct {
    uint16_t magic;
    uint8_t type;
    uint8_t reserved;
    uint32_t req_id;
    uint32_t length;  /* payload length */
} __attribute__((packed)) msg_header_t;

/* 文件分块元信息（嵌入 payload，多字节字段按网络字节序） */
typedef struct {
    uint32_t offset;      /* 本块在文件中的偏移 */
    uint32_t total_size;  /* 文件总大小 */
    uint8_t  flags;       /* FILE_FLAG_FINAL 等 */
} __attribute__((packed)) file_meta_t;

/* 打包消息到 buf，返回总字节数（含 header）。失败返回 -1。 */
int msg_pack(uint8_t *buf, size_t bufsize, msg_type_t type, uint32_t req_id,
             const void *payload, uint32_t length);

/* 从 buf 解析 header。成功返回 0，失败返回 -1。 */
int msg_parse_header(const uint8_t *buf, size_t buflen, msg_header_t *hdr);

/* 辅助函数：返回 type 对应的字符串名。 */
const char *msg_type_str(msg_type_t type);

/* 将文件分块元信息按网络字节序写入 dst（需 >= FILE_META_SIZE 字节）。 */
void file_meta_pack(uint8_t *dst, const file_meta_t *m);

/* 从 src 按网络字节序解析文件分块元信息（需 >= FILE_META_SIZE 字节）。 */
void file_meta_unpack(const uint8_t *src, file_meta_t *m);

#endif /* PROTOCOL_H */

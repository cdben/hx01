#include "protocol.h"

#include <string.h>
#include <arpa/inet.h>

/* 校验 type 是否为合法消息类型。 */
static int msg_type_valid(msg_type_t type)
{
    switch (type) {
    case MSG_REGISTER:
    case MSG_REGISTER_ACK:
    case MSG_CMD:
    case MSG_RESULT:
    case MSG_HEARTBEAT:
    case MSG_HEARTBEAT_ACK:
    case MSG_ERROR:
    case MSG_LIST:
    case MSG_LIST_RESP:
    case MSG_CANCEL:
    case MSG_FILE_UPLOAD:
    case MSG_FILE_DOWNLOAD:
    case MSG_FILE_DATA:
    case MSG_FILE_ACK:
    case MSG_AUTH_INIT:
    case MSG_AUTH_CHALLENGE:
    case MSG_AUTH_RESPONSE:
    case MSG_AUTH_OK:
    case MSG_AUTH_FAIL:
        return 1;
    default:
        return 0;
    }
}

int msg_pack(uint8_t *buf, size_t bufsize, msg_type_t type, uint32_t req_id,
             const void *payload, uint32_t length)
{
    uint16_t magic_net;
    uint32_t req_id_net;
    uint32_t length_net;
    uint32_t total;

    if (buf == NULL) {
        return -1;
    }

    /* length 为 0 时允许 payload 为 NULL，否则 payload 必须非空。 */
    if (length > 0 && payload == NULL) {
        return -1;
    }

    if (length > MAX_PAYLOAD) {
        return -1;
    }

    if (!msg_type_valid(type)) {
        return -1;
    }

    total = (uint32_t)HEADER_SIZE + length;
    if (bufsize < total) {
        return -1;
    }

    /* 按大端序写入 header 字段。 */
    magic_net = htons(MAGIC);
    req_id_net = htonl(req_id);
    length_net = htonl(length);

    memcpy(buf + 0, &magic_net, 2);
    buf[2] = (uint8_t)type;
    buf[3] = 0;  /* reserved */
    memcpy(buf + 4, &req_id_net, 4);
    memcpy(buf + 8, &length_net, 4);

    if (length > 0) {
        memcpy(buf + HEADER_SIZE, payload, length);
    }

    return (int)total;
}

int msg_parse_header(const uint8_t *buf, size_t buflen, msg_header_t *hdr)
{
    uint16_t magic_net;
    uint32_t req_id_net;
    uint32_t length_net;

    if (buf == NULL || hdr == NULL) {
        return -1;
    }

    if (buflen < HEADER_SIZE) {
        return -1;
    }

    memcpy(&magic_net, buf + 0, 2);
    memcpy(&req_id_net, buf + 4, 4);
    memcpy(&length_net, buf + 8, 4);

    hdr->magic = ntohs(magic_net);
    hdr->type = buf[2];
    hdr->reserved = buf[3];
    hdr->req_id = ntohl(req_id_net);
    hdr->length = ntohl(length_net);

    if (hdr->magic != MAGIC) {
        return -1;
    }

    if (!msg_type_valid((msg_type_t)hdr->type)) {
        return -1;
    }

    if (hdr->length > MAX_PAYLOAD) {
        return -1;
    }

    return 0;
}

const char *msg_type_str(msg_type_t type)
{
    switch (type) {
    case MSG_REGISTER:
        return "REGISTER";
    case MSG_REGISTER_ACK:
        return "REGISTER_ACK";
    case MSG_CMD:
        return "CMD";
    case MSG_RESULT:
        return "RESULT";
    case MSG_HEARTBEAT:
        return "HEARTBEAT";
    case MSG_HEARTBEAT_ACK:
        return "HEARTBEAT_ACK";
    case MSG_ERROR:
        return "ERROR";
    case MSG_LIST:
        return "LIST";
    case MSG_LIST_RESP:
        return "LIST_RESP";
    case MSG_CANCEL:
        return "CANCEL";
    case MSG_FILE_UPLOAD:
        return "FILE_UPLOAD";
    case MSG_FILE_DOWNLOAD:
        return "FILE_DOWNLOAD";
    case MSG_FILE_DATA:
        return "FILE_DATA";
    case MSG_FILE_ACK:
        return "FILE_ACK";
    case MSG_AUTH_INIT:
        return "AUTH_INIT";
    case MSG_AUTH_CHALLENGE:
        return "AUTH_CHALLENGE";
    case MSG_AUTH_RESPONSE:
        return "AUTH_RESPONSE";
    case MSG_AUTH_OK:
        return "AUTH_OK";
    case MSG_AUTH_FAIL:
        return "AUTH_FAIL";
    default:
        return "UNKNOWN";
    }
}

void file_meta_pack(uint8_t *dst, const file_meta_t *m)
{
    uint32_t offset_net;
    uint32_t total_net;

    if (dst == NULL || m == NULL) {
        return;
    }

    offset_net = htonl(m->offset);
    total_net = htonl(m->total_size);

    memcpy(dst + 0, &offset_net, 4);
    memcpy(dst + 4, &total_net, 4);
    dst[8] = m->flags;
}

void file_meta_unpack(const uint8_t *src, file_meta_t *m)
{
    uint32_t offset_net;
    uint32_t total_net;

    if (src == NULL || m == NULL) {
        return;
    }

    memcpy(&offset_net, src + 0, 4);
    memcpy(&total_net, src + 4, 4);

    m->offset = ntohl(offset_net);
    m->total_size = ntohl(total_net);
    m->flags = src[8];
}

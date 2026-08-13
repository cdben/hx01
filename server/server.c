/*
 * server.c - 远程管理服务端（中转枢纽，单线程 select）
 *
 * 职责：
 *   1. 监听端口，接受客户端（被控端）和管理端两类连接
 *   2. 客户端注册后维护 client_id <-> fd 映射
 *   3. 管理端发来的 CMD 转发给目标客户端
 *   4. 客户端返回的 RESULT 转发给所有管理端
 *   5. 心跳保活
 *
 * 用法: ./server [port]    默认端口 8888
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#include "protocol.h"
#include "utils.h"

#define DEFAULT_PORT 8888

/* 连接类型 */
typedef enum {
    CONN_TYPE_UNKNOWN = 0,
    CONN_TYPE_CLIENT  = 1,   /* 被控端 */
    CONN_TYPE_ADMIN   = 2    /* 管理端 */
} conn_type_t;

/* 每个连接的信息，以 fd 为索引 */
typedef struct {
    int          fd;
    conn_type_t  type;
    char         client_id[MAX_CLIENT_ID];
    int          registered;
} conn_info_t;

static conn_info_t conns[MAX_CLIENTS];

/* 初始化连接表：fd 置 -1 表示空槽（fd=0 是合法描述符） */
static void init_conns(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        conns[i].fd = -1;
        conns[i].type = CONN_TYPE_UNKNOWN;
        conns[i].client_id[0] = '\0';
        conns[i].registered = 0;
    }
}

/* 根据 client_id 查找 fd，找不到返回 -1 */
static int find_client_by_id(const char *id)
{
    int i;
    if (id == NULL || id[0] == '\0') {
        return -1;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (conns[i].fd >= 0 && conns[i].type == CONN_TYPE_CLIENT &&
            conns[i].registered &&
            strcmp(conns[i].client_id, id) == 0) {
            return i;  /* 索引即 fd */
        }
    }
    return -1;
}

/* 关闭连接并清理：close、清零 conns[fd] */
static void close_conn(int fd)
{
    if (fd < 0 || fd >= MAX_CLIENTS) {
        return;
    }

    if (conns[fd].registered && conns[fd].type == CONN_TYPE_CLIENT) {
        printf("[server] client disconnected: %s\n", conns[fd].client_id);
    }

    close(fd);

    conns[fd].fd = -1;
    conns[fd].type = CONN_TYPE_UNKNOWN;
    conns[fd].client_id[0] = '\0';
    conns[fd].registered = 0;
}

/* 处理新连接：accept、初始化 conns[fd]
 * 注：连接 socket 保持阻塞模式——recv_message/send_message 内部用
 * read_full/write_full 循环读写，阻塞模式才能正确处理被 TCP 拆分的
 * 大消息（如 64KB 的文件分块）。只有 listen socket 设为非阻塞（见 main）。 */
static void handle_new_connection(int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    /* 循环 accept 直到 EAGAIN，处理并发连接 */
    for (;;) {
        int fd = accept(listen_fd, (struct sockaddr *)&addr, &addr_len);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("[server] accept");
            }
            return;  /* 无更多待处理连接 */
        }

        if (fd >= MAX_CLIENTS) {
            fprintf(stderr, "[server] too many connections, rejecting fd=%d\n", fd);
            close(fd);
            continue;
        }

        conns[fd].fd = fd;
        conns[fd].type = CONN_TYPE_UNKNOWN;
        conns[fd].registered = 0;
        conns[fd].client_id[0] = '\0';

        printf("[server] new connection: fd=%d from %s:%d\n",
               fd, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    }
}

/* 将文件传输消息（UPLOAD/DOWNLOAD）按 payload 首段 client_id 路由转发到目标客户端；
 * 目标不存在时向管理端回 MSG_ERROR。 */
static void forward_file_to_client(int fd, msg_type_t type, uint32_t req_id,
                                   const uint8_t *payload, uint32_t payload_len)
{
    const char *target_id;
    int target_fd;

    if (memchr(payload, '\0', payload_len) == NULL) {
        fprintf(stderr, "[server] malformed file msg (no separator) from fd=%d\n", fd);
        return;
    }

    target_id = (const char *)payload;
    target_fd = find_client_by_id(target_id);
    if (target_fd < 0) {
        char errmsg[MAX_CLIENT_ID + 32];
        int elen = snprintf(errmsg, sizeof(errmsg),
                            "client '%s' not found", target_id);
        send_message(fd, MSG_ERROR, req_id, errmsg, (uint32_t)elen + 1);
        printf("[server] file transfer: client %s NOT FOUND\n", target_id);
        return;
    }

    if (send_message(target_fd, type, req_id, payload, payload_len) < 0) {
        perror("[server] forward file msg");
    }
    printf("[server] admin -> client %s file transfer (%s)\n",
           target_id, msg_type_str(type));
}

/* 处理已有连接的数据：recv_message 后按消息类型分发 */
static void handle_client_data(int fd)
{
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];
    msg_type_t type;
    uint32_t req_id;
    uint32_t payload_len;
    const uint8_t *payload;
    int ret;

    ret = recv_message(fd, buf, sizeof(buf), &type, &req_id, NULL, &payload_len);
    if (ret < 0) {
        fprintf(stderr, "[server] recv_message failed fd=%d errno=%d (%s)\n",
                fd, errno, strerror(errno));
        close_conn(fd);
        return;
    }
    /* payload 数据在 buf + HEADER_SIZE（传 NULL 时直接使用 buf 内部） */
    payload = buf + HEADER_SIZE;

    switch (type) {
    case MSG_REGISTER: {
        /* 客户端注册：payload 是 client_id 字符串（含 \0） */
        int id_len = (int)payload_len;
        if (id_len <= 0 || id_len >= MAX_CLIENT_ID) {
            fprintf(stderr, "[server] invalid register payload len=%d from fd=%d\n",
                    id_len, fd);
            close_conn(fd);
            return;
        }
        memcpy(conns[fd].client_id, payload, id_len);
        conns[fd].client_id[id_len] = '\0';
        conns[fd].type = CONN_TYPE_CLIENT;
        conns[fd].registered = 1;

        /* 回送 REGISTER_ACK */
        if (send_message(fd, MSG_REGISTER_ACK, 0, NULL, 0) < 0) {
            perror("[server] send REGISTER_ACK");
        }

        printf("[server] client registered: %s (fd=%d)\n",
               conns[fd].client_id, fd);
        break;
    }

    case MSG_CMD: {
        /* 管理端发来命令：payload 格式 = "target_client_id\0command" */
        const char *target_id;
        const char *cmd_str;
        int target_fd;

        conns[fd].type = CONN_TYPE_ADMIN;

        target_id = (const char *)payload;
        /* 在 payload 范围内查找 \0 分隔符 */
        cmd_str = (const char *)memchr(payload, '\0', payload_len);
        if (cmd_str == NULL) {
            fprintf(stderr, "[server] malformed CMD (no separator) from fd=%d\n", fd);
            return;
        }
        cmd_str++;  /* 跳过 \0，指向 command 部分 */

        target_fd = find_client_by_id(target_id);
        if (target_fd < 0) {
            /* 目标客户端不在线，回错误给管理端 */
            char errmsg[MAX_CLIENT_ID + 32];
            int elen = snprintf(errmsg, sizeof(errmsg),
                                "client '%s' not found", target_id);
            send_message(fd, MSG_ERROR, req_id, errmsg, (uint32_t)elen + 1);
            printf("[server] admin -> client %s NOT FOUND\n", target_id);
            return;
        }

        /* 转发 CMD 给目标客户端（保持原始 req_id 和原始 payload） */
        if (send_message(target_fd, MSG_CMD, req_id, payload, payload_len) < 0) {
            perror("[server] forward CMD");
        }
        printf("[server] admin -> client %s cmd: %s\n", target_id, cmd_str);
        break;
    }

    case MSG_RESULT: {
        /* 客户端返回执行结果：转发给所有管理端 */
        int i;
        const char *cid = conns[fd].registered ? conns[fd].client_id : "unknown";

        printf("[server] client %s -> admin result (req_id=%u)\n", cid, req_id);

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (i == fd) {
                continue;
            }
            if (conns[i].fd >= 0 && conns[i].type == CONN_TYPE_ADMIN) {
                if (send_message(conns[i].fd, MSG_RESULT, req_id,
                                 payload, payload_len) < 0) {
                    perror("[server] forward RESULT");
                }
            }
        }
        break;
    }

    case MSG_LIST:
        /* 管理端请求客户端列表，标记为管理端并回复 */
        {
            char list_buf[MAX_PAYLOAD];
            int pos = 0;
            int j;

            conns[fd].type = CONN_TYPE_ADMIN;
            printf("[server] admin (fd=%d) requested client list\n", fd);

            /* 枚举所有已注册客户端，格式: "id1\nid2\n..." */
            list_buf[0] = '\0';
            for (j = 0; j < MAX_CLIENTS; j++) {
                if (conns[j].fd >= 0 && conns[j].type == CONN_TYPE_CLIENT &&
                    conns[j].registered) {
                    if (pos > 0) {
                        list_buf[pos++] = '\n';
                    }
                    size_t id_len = strlen(conns[j].client_id);
                    if ((size_t)pos + id_len >= sizeof(list_buf) - 1) {
                        break;
                    }
                    memcpy(list_buf + pos, conns[j].client_id, id_len);
                    pos += (int)id_len;
                }
            }

            if (send_message(fd, MSG_LIST_RESP, 0, list_buf,
                             (uint32_t)pos + 1) < 0) {
                perror("[server] send LIST_RESP");
            }
        }
        break;

    case MSG_CANCEL: {
        /* 管理端取消正在执行的命令，转发到目标客户端 */
        const char *target_id = (const char *)payload;

        conns[fd].type = CONN_TYPE_ADMIN;

        int target_fd = find_client_by_id(target_id);
        if (target_fd < 0) {
            printf("[server] CANCEL: client '%s' not found\n", target_id);
            return;
        }

        if (send_message(target_fd, MSG_CANCEL, req_id, NULL, 0) < 0) {
            perror("[server] forward CANCEL");
        }
        printf("[server] CANCEL forwarded to client %s\n", target_id);
        break;
    }

    case MSG_FILE_UPLOAD:
    case MSG_FILE_DOWNLOAD:
        /* 管理端发来的文件传输请求：按 client_id 路由转发到目标客户端 */
        conns[fd].type = CONN_TYPE_ADMIN;
        forward_file_to_client(fd, type, req_id, payload, payload_len);
        break;

    case MSG_FILE_DATA:
    case MSG_FILE_ACK: {
        /* 客户端回传的文件数据/应答：广播给所有管理端 */
        int i;
        const char *cid = conns[fd].registered ? conns[fd].client_id : "unknown";

        printf("[server] client %s -> admin %s (req_id=%u)\n",
               cid, msg_type_str(type), req_id);

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (i == fd) {
                continue;
            }
            if (conns[i].fd >= 0 && conns[i].type == CONN_TYPE_ADMIN) {
                if (send_message(conns[i].fd, type, req_id,
                                 payload, payload_len) < 0) {
                    perror("[server] forward file msg");
                }
            }
        }
        break;
    }

    case MSG_HEARTBEAT:
        /* 心跳，回 ACK */
        if (send_message(fd, MSG_HEARTBEAT_ACK, 0, NULL, 0) < 0) {
            perror("[server] send HEARTBEAT_ACK");
        }
        break;

    default:
        fprintf(stderr, "[server] unknown message type %d from fd=%d\n",
                (int)type, fd);
        break;
    }
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int listen_fd, i;
    struct sockaddr_in addr;
    int opt = 1;
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Usage: %s <port>\n", argv[0]);
            return 1;
        }
    }

    /* 忽略 SIGPIPE（write 到已关闭的 socket 时触发） */
    signal(SIGPIPE, SIG_IGN);

    init_conns();

    /* 创建监听 socket */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("[server] socket");
        return 1;
    }

    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[server] bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("[server] listen");
        close(listen_fd);
        return 1;
    }

    if (set_nonblock(listen_fd) < 0) {
        perror("[server] set_nonblock listen");
        close(listen_fd);
        return 1;
    }

    printf("[server] listening on port %d (select, single-thread)\n", port);

    /* 事件循环 */
    for (;;) {
        fd_set rset;
        int maxfd;
        int ret;

        FD_ZERO(&rset);
        FD_SET(listen_fd, &rset);
        maxfd = listen_fd;

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (conns[i].fd >= 0) {
                FD_SET(conns[i].fd, &rset);
                if (conns[i].fd > maxfd) {
                    maxfd = conns[i].fd;
                }
            }
        }

        ret = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[server] select");
            break;
        }

        if (FD_ISSET(listen_fd, &rset)) {
            handle_new_connection(listen_fd);
        }

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (conns[i].fd >= 0 && FD_ISSET(conns[i].fd, &rset)) {
                handle_client_data(conns[i].fd);
            }
        }
    }

    close(listen_fd);
    return 0;
}

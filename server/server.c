/*
 * server.c - 远程管理服务端（中转枢纽，单线程 epoll）
 *
 * 职责：
 *   1. 监听端口，接受客户端（被控端）和管理端两类连接
 *   2. 客户端注册后维护 client_id <-> fd 映射
 *   3. 管理端发来的 CMD 转发给目标客户端
 *   4. 客户端返回的 RESULT 转发给所有管理端
 *   5. 心跳保活
 *
 * 用法: ./server [port]    默认端口 8888
 *
 * IO 模型：epoll 边缘触发（ET），非阻塞 socket。每条连接维护独立的
 * 读/写缓冲区以处理 TCP 拆包/粘包与发送积压。fd 仍用作 conns[] 下标，
 * 配合 epoll_data.ptr 直接定位连接结构，避免遍历扫描。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>

#include "protocol.h"
#include "utils.h"

#define DEFAULT_PORT 8888
#define LISTEN_BACKLOG 4096        /* listen backlog，应对瞬时并发重连 */
#define EPOLL_EVENTS   1024        /* 单次 epoll_wait 取出的事件数 */
#define READ_CHUNK     16384       /* 单次 read 尝试读取量 */
#define SWEEP_INTERVAL 10          /* 超时扫描间隔（秒），epoll_wait 超时 */
#define CLIENT_TIMEOUT 45          /* 客户端无活动超时（秒），3 个心跳周期 */

/* 单条消息最大字节数：header + payload */
#define MAX_MSG_SIZE   (HEADER_SIZE + MAX_PAYLOAD)

/* 连接类型 */
typedef enum {
    CONN_TYPE_UNKNOWN = 0,
    CONN_TYPE_CLIENT  = 1,   /* 被控端 */
    CONN_TYPE_ADMIN   = 2    /* 管理端 */
} conn_type_t;

/* 写缓冲节点：链式队列，存放待发送的完整消息 */
typedef struct write_buf {
    struct write_buf *next;
    size_t   total;     /* 本节点总字节数 */
    size_t   sent;      /* 已发送字节数 */
    uint8_t  data[];    /* 柔性数组，存 header+payload */
} write_buf_t;

/* 每个连接的信息，以 fd 为索引 */
typedef struct {
    int          fd;
    conn_type_t  type;
    char         client_id[MAX_CLIENT_ID];
    int          registered;
    time_t       last_active;   /* 最后一次收到数据的时间，用于心跳超时 */

    /* 读缓冲：拼接未读完的半包 */
    uint8_t     *rbuf;          /* 读缓冲，容量 MAX_MSG_SIZE */
    size_t       rlen;          /* 当前已读入字节数 */

    /* 写缓冲队列：非阻塞写时若 EAGAIN 则入队，等 EPOLLOUT 再发 */
    write_buf_t *whead;
    write_buf_t *wtail;
} conn_info_t;

static conn_info_t conns[MAX_CLIENTS];

/* admin 链表：广播 RESULT/FILE_DATA 时遍历，避免扫全表 */
static int admin_fds[MAX_CLIENTS];
static int admin_count = 0;

/* 初始化连接表：fd 置 -1 表示空槽（fd=0 是合法描述符） */
static void init_conns(void)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        conns[i].fd = -1;
        conns[i].type = CONN_TYPE_UNKNOWN;
        conns[i].client_id[0] = '\0';
        conns[i].registered = 0;
        conns[i].last_active = 0;
        conns[i].rbuf = NULL;
        conns[i].rlen = 0;
        conns[i].whead = NULL;
        conns[i].wtail = NULL;
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

/* 注册/注销 admin 到广播链表 */
static void admin_register(int fd)
{
    int i;
    for (i = 0; i < admin_count; i++) {
        if (admin_fds[i] == fd) {
            return;  /* 已在 */
        }
    }
    if (admin_count < MAX_CLIENTS) {
        admin_fds[admin_count++] = fd;
    }
}

static void admin_unregister(int fd)
{
    int i;
    for (i = 0; i < admin_count; i++) {
        if (admin_fds[i] == fd) {
            admin_fds[i] = admin_fds[admin_count - 1];
            admin_count--;
            return;
        }
    }
}

/* 释放写缓冲队列 */
static void write_queue_free(conn_info_t *c)
{
    write_buf_t *wb = c->whead;
    while (wb != NULL) {
        write_buf_t *next = wb->next;
        free(wb);
        wb = next;
    }
    c->whead = NULL;
    c->wtail = NULL;
}

/* 关闭连接并清理：close、清零 conns[fd]、从 epoll 摘除 */
static void close_conn(int fd, int epfd)
{
    if (fd < 0 || fd >= MAX_CLIENTS) {
        return;
    }

    conn_info_t *c = &conns[fd];

    if (c->registered && c->type == CONN_TYPE_CLIENT) {
        printf("[server] client disconnected: %s\n", c->client_id);
    }
    if (c->type == CONN_TYPE_ADMIN) {
        admin_unregister(fd);
    }

    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);

    free(c->rbuf);
    write_queue_free(c);

    c->fd = -1;
    c->type = CONN_TYPE_UNKNOWN;
    c->client_id[0] = '\0';
    c->registered = 0;
    c->last_active = 0;
    c->rbuf = NULL;
    c->rlen = 0;
}

/* 处理新连接：accept、设非阻塞、加入 epoll(ET)、初始化 conns[fd] */
static void handle_new_connection(int listen_fd, int epfd)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    /* 循环 accept 直到 EAGAIN，处理并发连接（listen socket 非阻塞） */
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

        /* 连接 socket 设为非阻塞 */
        if (set_nonblock(fd) < 0) {
            perror("[server] set_nonblock conn");
            close(fd);
            continue;
        }

        conn_info_t *c = &conns[fd];
        c->fd = fd;
        c->type = CONN_TYPE_UNKNOWN;
        c->registered = 0;
        c->last_active = time(NULL);
        c->client_id[0] = '\0';
        c->rbuf = malloc(MAX_MSG_SIZE);
        if (c->rbuf == NULL) {
            fprintf(stderr, "[server] out of memory for rbuf fd=%d\n", fd);
            close(fd);
            c->fd = -1;
            continue;
        }
        c->rlen = 0;
        c->whead = NULL;
        c->wtail = NULL;

        /* 加入 epoll：边缘触发，监听可读 */
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.ptr = c;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("[server] epoll_ctl add");
            free(c->rbuf);
            close(fd);
            c->fd = -1;
            c->rbuf = NULL;
            continue;
        }

        printf("[server] new connection: fd=%d from %s:%d\n",
               fd, inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));
    }
}

/*
 * 尝试把一条完整消息写入连接。非阻塞写：
 *   - 能直接写完则立即写并返回
 *   - 写不完或 EAGAIN 则把剩余部分挂到写缓冲队列，由 EPOLLOUT 驱动
 * 返回 0 成功（已发送或已入队），-1 连接已断开。
 */
static int conn_send(int fd, msg_type_t type, uint32_t req_id,
                     const void *payload, uint32_t length, int epfd)
{
    conn_info_t *c = &conns[fd];

    /* 先打包 */
    write_buf_t *wb = malloc(sizeof(write_buf_t) + HEADER_SIZE + length);
    if (wb == NULL) {
        return -1;
    }
    wb->next = NULL;
    wb->sent = 0;
    int total = msg_pack(wb->data, HEADER_SIZE + length, type, req_id, payload, length);
    if (total < 0) {
        free(wb);
        return -1;
    }
    wb->total = (size_t)total;

    /* 若队列里已有积压，直接入队，不尝试写（保持顺序） */
    if (c->whead != NULL) {
        c->wtail->next = wb;
        c->wtail = wb;
        return 0;
    }

    /* 队列空，尝试直接写 */
    ssize_t n = write(fd, wb->data, wb->total);
    if (n < 0) {
        if (errno == EINTR) {
            n = 0;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            n = 0;
        } else {
            free(wb);
            return -1;  /* 连接出错 */
        }
    }
    wb->sent = (size_t)n;

    if (wb->sent < wb->total) {
        /* 没写完，入队并开启 EPOLLOUT */
        c->whead = wb;
        c->wtail = wb;
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = c;
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    } else {
        /* 一次写完，无需入队 */
        free(wb);
    }
    return 0;
}

/* 刷写缓冲队列。EPOLLOUT 触发时调用。返回 0 正常，-1 连接断开。 */
static int flush_write_queue(int fd, int epfd)
{
    conn_info_t *c = &conns[fd];
    write_buf_t *wb = c->whead;

    while (wb != NULL) {
        while (wb->sent < wb->total) {
            ssize_t n = write(fd, wb->data + wb->sent, wb->total - wb->sent);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;  /* 写缓冲满，等下次 EPOLLOUT */
                }
                return -1;  /* 出错 */
            }
            wb->sent += (size_t)n;
        }
        /* 本节点发完，出队 */
        c->whead = wb->next;
        free(wb);
        wb = c->whead;
    }
    c->wtail = NULL;

    /* 队列空了，关掉 EPOLLOUT，只留 EPOLLIN */
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = c;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    return 0;
}

/* 广播一条消息给所有 admin */
static void broadcast_to_admins(msg_type_t type, uint32_t req_id,
                                const uint8_t *payload, uint32_t payload_len, int epfd)
{
    int i;
    for (i = 0; i < admin_count; i++) {
        int afd = admin_fds[i];
        if (conns[afd].fd >= 0) {
            if (conn_send(afd, type, req_id, payload, payload_len, epfd) < 0) {
                close_conn(afd, epfd);
            }
        }
    }
}

/* 将文件传输消息（UPLOAD/DOWNLOAD）按 payload 首段 client_id 路由转发到目标客户端；
 * 目标不存在时向管理端回 MSG_ERROR。 */
static void forward_file_to_client(int fd, int epfd, msg_type_t type, uint32_t req_id,
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
        conn_send(fd, MSG_ERROR, req_id, errmsg, (uint32_t)elen + 1, epfd);
        printf("[server] file transfer: client %s NOT FOUND\n", target_id);
        return;
    }

    if (conn_send(target_fd, type, req_id, payload, payload_len, epfd) < 0) {
        fprintf(stderr, "[server] forward file msg failed fd=%d\n", target_fd);
    }
    printf("[server] admin -> client %s file transfer (%s)\n",
           target_id, msg_type_str(type));
}

/* 处理一条完整消息（header+payload 已在 rbuf 中）。按消息类型分发。 */
static void handle_message(int fd, int epfd, msg_type_t type, uint32_t req_id,
                           uint8_t *payload, uint32_t payload_len)
{
    conn_info_t *c = &conns[fd];

    switch (type) {
    case MSG_REGISTER: {
        /* 客户端注册：payload 是 client_id 字符串（含 \0） */
        int id_len = (int)payload_len;
        if (id_len <= 0 || id_len >= MAX_CLIENT_ID) {
            fprintf(stderr, "[server] invalid register payload len=%d from fd=%d\n",
                    id_len, fd);
            close_conn(fd, epfd);
            return;
        }
        memcpy(c->client_id, payload, id_len);
        c->client_id[id_len] = '\0';
        c->type = CONN_TYPE_CLIENT;
        c->registered = 1;

        /* 回送 REGISTER_ACK */
        if (conn_send(fd, MSG_REGISTER_ACK, 0, NULL, 0, epfd) < 0) {
            close_conn(fd, epfd);
            return;
        }

        printf("[server] client registered: %s (fd=%d)\n", c->client_id, fd);
        break;
    }

    case MSG_CMD: {
        /* 管理端发来命令：payload 格式 = "target_client_id\0command" */
        const char *target_id;
        const char *cmd_str;
        int target_fd;

        if (c->type != CONN_TYPE_ADMIN) {
            c->type = CONN_TYPE_ADMIN;
            admin_register(fd);
        }

        target_id = (const char *)payload;
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
            conn_send(fd, MSG_ERROR, req_id, errmsg, (uint32_t)elen + 1, epfd);
            printf("[server] admin -> client %s NOT FOUND\n", target_id);
            return;
        }

        /* 转发 CMD 给目标客户端（保持原始 req_id 和原始 payload） */
        if (conn_send(target_fd, MSG_CMD, req_id, payload, payload_len, epfd) < 0) {
            fprintf(stderr, "[server] forward CMD failed fd=%d\n", target_fd);
        }
        printf("[server] admin -> client %s cmd: %s\n", target_id, cmd_str);
        break;
    }

    case MSG_RESULT: {
        /* 客户端返回执行结果：转发给所有管理端 */
        const char *cid = c->registered ? c->client_id : "unknown";
        printf("[server] client %s -> admin result (req_id=%u)\n", cid, req_id);
        broadcast_to_admins(MSG_RESULT, req_id, payload, payload_len, epfd);
        break;
    }

    case MSG_LIST: {
        /* 管理端请求客户端列表，标记为管理端并回复 */
        char list_buf[MAX_PAYLOAD];
        int pos = 0;
        int j;

        if (c->type != CONN_TYPE_ADMIN) {
            c->type = CONN_TYPE_ADMIN;
            admin_register(fd);
        }
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

        if (conn_send(fd, MSG_LIST_RESP, 0, list_buf,
                      (uint32_t)pos + 1, epfd) < 0) {
            close_conn(fd, epfd);
        }
        break;
    }

    case MSG_CANCEL: {
        /* 管理端取消正在执行的命令，转发到目标客户端 */
        const char *target_id = (const char *)payload;

        if (c->type != CONN_TYPE_ADMIN) {
            c->type = CONN_TYPE_ADMIN;
            admin_register(fd);
        }

        int target_fd = find_client_by_id(target_id);
        if (target_fd < 0) {
            printf("[server] CANCEL: client '%s' not found\n", target_id);
            return;
        }

        if (conn_send(target_fd, MSG_CANCEL, req_id, NULL, 0, epfd) < 0) {
            fprintf(stderr, "[server] forward CANCEL failed fd=%d\n", target_fd);
        }
        printf("[server] CANCEL forwarded to client %s\n", target_id);
        break;
    }

    case MSG_FILE_UPLOAD:
    case MSG_FILE_DOWNLOAD:
        /* 管理端发来的文件传输请求：按 client_id 路由转发到目标客户端 */
        if (c->type != CONN_TYPE_ADMIN) {
            c->type = CONN_TYPE_ADMIN;
            admin_register(fd);
        }
        forward_file_to_client(fd, epfd, type, req_id, payload, payload_len);
        break;

    case MSG_FILE_DATA:
    case MSG_FILE_ACK: {
        /* 客户端回传的文件数据/应答：广播给所有管理端 */
        const char *cid = c->registered ? c->client_id : "unknown";
        printf("[server] client %s -> admin %s (req_id=%u)\n",
               cid, msg_type_str(type), req_id);
        broadcast_to_admins(type, req_id, payload, payload_len, epfd);
        break;
    }

    case MSG_HEARTBEAT:
        /* 心跳，回 ACK */
        if (conn_send(fd, MSG_HEARTBEAT_ACK, 0, NULL, 0, epfd) < 0) {
            close_conn(fd, epfd);
        }
        break;

    default:
        fprintf(stderr, "[server] unknown message type %d from fd=%d\n",
                (int)type, fd);
        break;
    }
}

/*
 * 处理已有连接的可读事件：边缘触发，循环 read 直到 EAGAIN。
 * 读到的字节拼到 rbuf，每凑够一条完整消息就分发。
 */
static void handle_client_data(int fd, int epfd)
{
    conn_info_t *c = &conns[fd];

    if (c->rbuf == NULL) {
        return;
    }

    for (;;) {
        /* 读到缓冲尾部 */
        ssize_t n = read(fd, c->rbuf + c->rlen, MAX_MSG_SIZE - c->rlen);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  /* 读完了，等下次事件 */
            }
            /* 出错，关闭 */
            close_conn(fd, epfd);
            return;
        }
        if (n == 0) {
            /* 对端关闭 */
            close_conn(fd, epfd);
            return;
        }
        c->last_active = time(NULL);  /* 收到数据，刷新活跃时间 */
        c->rlen += (size_t)n;

        /* 尝试从 rbuf 中解析出尽可能多的完整消息 */
        for (;;) {
            if (c->rlen < HEADER_SIZE) {
                break;  /* header 都没读满，等更多数据 */
            }
            msg_header_t hdr;
            if (msg_parse_header(c->rbuf, HEADER_SIZE, &hdr) != 0) {
                /* header 非法，丢弃连接 */
                close_conn(fd, epfd);
                return;
            }
            size_t msg_size = HEADER_SIZE + hdr.length;
            if (c->rlen < msg_size) {
                break;  /* payload 还没读满，等更多数据 */
            }
            /* 一条完整消息就绪 */
            uint8_t *payload = c->rbuf + HEADER_SIZE;
            handle_message(fd, epfd, (msg_type_t)hdr.type, hdr.req_id,
                           payload, hdr.length);

            /* 若连接已被 handle_message 关闭，直接返回 */
            if (conns[fd].fd < 0) {
                return;
            }

            /* 把剩余字节移到缓冲头部，继续解析下一条 */
            size_t left = c->rlen - msg_size;
            if (left > 0) {
                memmove(c->rbuf, c->rbuf + msg_size, left);
            }
            c->rlen = left;
        }
    }
}

/* 扫描所有连接，清理超过 CLIENT_TIMEOUT 秒无活动的 client。
 * admin 不做超时清理（管理端可能长时间只等结果不发包）。 */
static void sweep_timeout(int epfd)
{
    time_t now = time(NULL);
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (conns[i].fd >= 0 && conns[i].type == CONN_TYPE_CLIENT &&
            conns[i].registered) {
            if (now - conns[i].last_active > CLIENT_TIMEOUT) {
                printf("[server] client %s timeout (no activity %lds), closing\n",
                       conns[i].client_id,
                       (long)(now - conns[i].last_active));
                close_conn(i, epfd);
            }
        }
    }
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    int listen_fd, epfd, nfds, i;
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

    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        perror("[server] listen");
        close(listen_fd);
        return 1;
    }

    if (set_nonblock(listen_fd) < 0) {
        perror("[server] set_nonblock listen");
        close(listen_fd);
        return 1;
    }

    /* 创建 epoll 实例 */
    epfd = epoll_create1(0);
    if (epfd < 0) {
        perror("[server] epoll_create1");
        close(listen_fd);
        return 1;
    }

    /* 监听 socket 加入 epoll，边缘触发 */
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = NULL;  /* listen socket 用 NULL 标记 */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("[server] epoll_ctl listen");
        close(epfd);
        close(listen_fd);
        return 1;
    }

    printf("[server] listening on port %d (epoll ET, single-thread)\n", port);

    struct epoll_event events[EPOLL_EVENTS];

    /* 事件循环：epoll_wait 带 SWEEP_INTERVAL 秒超时，用于定期清理失联 client */
    for (;;) {
        nfds = epoll_wait(epfd, events, EPOLL_EVENTS, SWEEP_INTERVAL * 1000);
        if (nfds < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("[server] epoll_wait");
            break;
        }

        if (nfds == 0) {
            /* epoll_wait 超时：扫描清理失联 client */
            sweep_timeout(epfd);
            continue;
        }

        for (i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                /* listen socket 可读：新连接 */
                handle_new_connection(listen_fd, epfd);
                continue;
            }

            conn_info_t *c = (conn_info_t *)events[i].data.ptr;
            int fd = c->fd;
            if (fd < 0) {
                continue;
            }

            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                close_conn(fd, epfd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                handle_client_data(fd, epfd);
                /* handle_client_data 可能关闭连接，需复查 */
                if (conns[fd].fd < 0) {
                    continue;
                }
            }

            if (events[i].events & EPOLLOUT) {
                if (flush_write_queue(fd, epfd) < 0) {
                    close_conn(fd, epfd);
                }
            }
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}

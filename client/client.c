/*
 * client.c - 远程管理客户端（被控端）
 *
 * 职责：
 *   1. 主动连接服务端（反向连接）
 *   2. 注册自己的 client_id
 *   3. 接收服务端转发的命令，fork/exec 执行，回传结果
 *   4. 支持 MSG_CANCEL 中断正在执行的命令
 *   5. 定时发心跳保活
 *   6. 断线重连（指数退避）
 *
 * 用法: ./client <server_ip> <server_port> <client_id>
 * 示例: ./client 127.0.0.1 8888 web-01
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
#include <netdb.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>

#include "protocol.h"
#include "utils.h"

#define HEARTBEAT_INTERVAL 15   /* 心跳间隔（秒） */
#define MAX_RETRY_DELAY   60    /* 最大重连间隔 */
#define OUTPUT_CHUNK      4096  /* 每次从管道读取的块大小 */

/* ---- 子进程状态（异步执行） ---- */
static pid_t    child_pid     = -1;
static int      child_pipe    = -1;   /* 读取子进程 stdout+stderr 的管道 */
static char    *child_out     = NULL; /* 输出缓冲区 */
static size_t   child_out_len = 0;
static size_t   child_out_cap = 0;
static uint32_t child_req_id  = 0;

/* ---- 文件上传状态 ---- */
static int upload_fd = -1;  /* 当前上传目标文件句柄（-1 表示未打开） */

/* 启动并重置输出缓冲区 */
static void output_buf_reset(void)
{
    free(child_out);
    child_out     = NULL;
    child_out_len = 0;
    child_out_cap = 0;
}

/* 追加数据到输出缓冲区 */
static int output_buf_append(const char *data, size_t len)
{
    size_t need = child_out_len + len + 1;  /* +1 for \0 */
    if (need > child_out_cap) {
        size_t new_cap = child_out_cap ? child_out_cap * 2 : 8192;
        while (new_cap < need && new_cap < MAX_RESULT_LEN) {
            new_cap *= 2;
        }
        if (new_cap > MAX_RESULT_LEN) {
            new_cap = MAX_RESULT_LEN;
        }
        if (need > new_cap) {
            return -1;  /* 输出超限 */
        }
        char *p = realloc(child_out, new_cap);
        if (p == NULL) {
            return -1;
        }
        child_out = p;
        child_out_cap = new_cap;
    }
    memcpy(child_out + child_out_len, data, len);
    child_out_len += len;
    child_out[child_out_len] = '\0';
    return 0;
}

/*
 * fork/exec 子进程，执行包装后的 shell 命令。
 * 返回 0 成功，-1 失败。
 */
static int start_child(const char *wrapped_cmd)
{
    int pipefd[2];

    if (pipe(pipefd) < 0) {
        perror("[client] pipe");
        return -1;
    }

    child_pid = fork();
    if (child_pid < 0) {
        perror("[client] fork");
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (child_pid == 0) {
        /* ---- 子进程 ---- */
        /* 创建独立进程组，以便后续 kill(-pgid) 一次性清理所有子孙进程 */
        setpgid(0, 0);

        close(pipefd[0]);                     /* 关闭读端 */
        dup2(pipefd[1], STDOUT_FILENO);       /* stdout → 管道 */
        dup2(pipefd[1], STDERR_FILENO);       /* stderr → 管道（已在 wrapped_cmd 中用 2>&1 合并） */
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", wrapped_cmd, (char *)NULL);
        /* execl 失败 */
        fprintf(stderr, "exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* ---- 父进程 ---- */
    /* 父进程也设一次进程组，消除 race condition（子进程可能尚未执行 setpgid） */
    setpgid(child_pid, child_pid);

    close(pipefd[1]);  /* 关闭写端 */
    child_pipe = pipefd[0];
    set_nonblock(child_pipe);

    output_buf_reset();
    return 0;
}

/* 非阻塞读取子进程输出，追加到缓冲区 */
static void read_child_output(void)
{
    char chunk[OUTPUT_CHUNK];

    if (child_pipe < 0) {
        return;
    }

    for (;;) {
        ssize_t n = read(child_pipe, chunk, sizeof(chunk));
        if (n > 0) {
            output_buf_append(chunk, (size_t)n);
        } else {
            break;  /* EAGAIN 或 EOF */
        }
    }
}

/* 尝试回收子进程。返回 1 表示子进程已退出，0 表示还在运行。 */
static int check_child_exit(int *exit_code)
{
    int status;
    pid_t r;

    if (child_pid < 0) {
        return 1;
    }

    r = waitpid(child_pid, &status, WNOHANG);
    if (r == 0) {
        return 0;  /* 仍在运行 */
    }

    /* 子进程已退出 */
    if (r < 0) {
        *exit_code = -1;
    } else if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        *exit_code = -1;  /* 被信号终止 */
    } else {
        *exit_code = -1;
    }

    /* 读走管道中剩余数据 */
    read_child_output();

    /* 清理 */
    if (child_pipe >= 0) {
        close(child_pipe);
        child_pipe = -1;
    }
    child_pid = -1;

    return 1;
}

/* 杀死子进程及其所有子孙进程（通过进程组） */
static void kill_child(void)
{
    if (child_pid > 0) {
        /* 向整个进程组发 SIGTERM（负 pid = 进程组） */
        kill(-child_pid, SIGTERM);
        /* 等一会儿让进程优雅退出 */
        usleep(200000);  /* 200ms */
        /* 如果还没退，强制杀 */
        if (waitpid(child_pid, NULL, WNOHANG) == 0) {
            kill(-child_pid, SIGKILL);
            waitpid(child_pid, NULL, 0);  /* 阻塞等 */
        }
        /* 读走管道中剩余数据 */
        read_child_output();
        if (child_pipe >= 0) {
            close(child_pipe);
            child_pipe = -1;
        }
        child_pid = -1;
    }
}

/*
 * 从输出缓冲区中提取 CWD 并剥离 CWD 行。
 * 输出格式: ... __HX_CWD__:<path>\n
 * new_cwd 写出提取的路径，缓冲区中的 CWD 行被移除。
 */
static void extract_cwd_from_output(char *new_cwd, size_t new_cwd_size)
{
    char *marker;

    if (child_out == NULL || child_out_len == 0) {
        /* 没有输出，保持 CWD 不变（调用方会传入原 cwd） */
        return;
    }

    marker = strstr(child_out, "__HX_CWD__:");
    if (marker == NULL) {
        return;
    }

    /* 提取路径 */
    const char *path_start = marker + 11;  /* strlen("__HX_CWD__:") = 11 */
    char *path_end = strchr(path_start, '\n');
    if (path_end != NULL) {
        *path_end = '\0';
    }
    size_t plen = strlen(path_start);
    if (plen > 0 && path_start[plen - 1] == '\r') {
        ((char *)path_start)[plen - 1] = '\0';
    }

    strncpy(new_cwd, path_start, new_cwd_size - 1);
    new_cwd[new_cwd_size - 1] = '\0';

    /* 剥离 CWD 行 */
    *marker = '\0';
    if (marker > child_out && *(marker - 1) == '\n') {
        *(marker - 1) = '\0';
    }
    child_out_len = strlen(child_out);
}

/*
 * 构造 result 并发送：cwd\nexit_code\n<output>
 */
static void send_result(int sock_fd, const char *cwd, int exit_code,
                        uint32_t req_id, int was_cancelled)
{
    char result[MAX_RESULT_LEN];
    size_t pos = 0;

    /* 写 cwd */
    size_t cwd_len = strlen(cwd);
    if (cwd_len >= sizeof(result) - 1) {
        cwd_len = sizeof(result) - 1;
    }
    memcpy(result, cwd, cwd_len);
    pos = cwd_len;
    result[pos++] = '\n';

    /* 写 exit_code */
    int n = snprintf(result + pos, sizeof(result) - pos, "%d\n", exit_code);
    if (n < 0) n = 0;
    pos += (size_t)n;

    /* 写取消提示和输出 */
    if (was_cancelled) {
        const char *msg = "[cancelled]\n";
        size_t msg_len = strlen(msg);
        if (pos + msg_len < sizeof(result)) {
            memcpy(result + pos, msg, msg_len);
            pos += msg_len;
        }
    }

    if (child_out != NULL && child_out_len > 0 && pos < sizeof(result) - 1) {
        size_t remain = sizeof(result) - pos - 1;
        size_t copy_len = child_out_len < remain ? child_out_len : remain;
        memcpy(result + pos, child_out, copy_len);
        pos += copy_len;
    }

    result[pos] = '\0';

    if (send_message(sock_fd, MSG_RESULT, req_id, result,
                     (uint32_t)strlen(result) + 1) < 0) {
        fprintf(stderr, "[client] send RESULT failed\n");
    }

    output_buf_reset();
}

/* 发送文件传输应答：status 0 成功，负值为错误（errmsg 为描述）。 */
static void send_file_ack(int sock_fd, uint32_t req_id, int status,
                          const char *errmsg)
{
    uint8_t ack[256];
    int32_t status_net = htonl(status);
    const char *msg = errmsg != NULL ? errmsg : "";
    size_t len = strlen(msg);
    if (len > sizeof(ack) - 4 - 1) {
        len = sizeof(ack) - 4 - 1;
    }

    memcpy(ack, &status_net, 4);
    memcpy(ack + 4, msg, len);
    ack[4 + len] = '\0';

    if (send_message(sock_fd, MSG_FILE_ACK, req_id, ack,
                     (uint32_t)(4 + len + 1)) < 0) {
        fprintf(stderr, "[client] send FILE_ACK failed\n");
    }
}

/* 连接到服务端，返回 fd 或 -1 */
static int connect_server(const char *host, int port)
{
    int fd = -1;
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    int gai_rc;

    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;        /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;

    gai_rc = getaddrinfo(host, port_str, &hints, &res);
    if (gai_rc != 0) {
        /* getaddrinfo 失败不设 errno，用 gai_strerror 打印真实原因 */
        fprintf(stderr, "[client] resolve '%s' failed: %s\n",
                host, gai_strerror(gai_rc));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;  /* 连接成功 */
        }
        /* 连接失败，errno 此时是真实的（如 connection refused / timeout） */
        close(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0 && errno == 0) {
        /* 所有候选地址都 socket 失败但没设 errno 的兜底 */
        errno = ECONNREFUSED;
    }
    return fd;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-d] [-p pidfile] [-l logfile] <server_ip> <server_port> <client_id>\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -d            Run as daemon (background)\n");
    fprintf(stderr, "  -p <file>     Write PID to file\n");
    fprintf(stderr, "  -l <file>     Redirect stdout/stderr to log file\n");
    fprintf(stderr, "Example: %s 127.0.0.1 8888 web-01\n", prog);
    fprintf(stderr, "         %s -d -p /var/run/hx01.pid 10.0.0.1 8888 web-01\n", prog);
}

/*
 * 守护进程化：fork → setsid → fork → chdir → 重定向 stdio。
 * logfile 为 NULL 时重定向到 /dev/null。
 * 成功返回（在子进程中），失败退出进程。
 */
static void daemonize(const char *pidfile, const char *logfile)
{
    pid_t pid;

    /* 第一次 fork */
    pid = fork();
    if (pid < 0) {
        perror("daemon: fork");
        exit(1);
    }
    if (pid > 0) {
        _exit(0);  /* 父进程退出 */
    }

    /* 创建新会话，脱离控制终端 */
    if (setsid() < 0) {
        perror("daemon: setsid");
        exit(1);
    }

    /* 第二次 fork，防止重新获得终端 */
    pid = fork();
    if (pid < 0) {
        perror("daemon: fork2");
        exit(1);
    }
    if (pid > 0) {
        _exit(0);
    }

    /* 切换工作目录到根，避免占用挂载点 */
    chdir("/");

    /* 重定向标准流 */
    if (logfile != NULL) {
        /* 追加模式打开日志文件 */
        int lfd = open(logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd < 0) {
            perror("daemon: open logfile");
            exit(1);
        }
        dup2(lfd, STDOUT_FILENO);
        dup2(lfd, STDERR_FILENO);
        close(lfd);
    } else {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) {
                close(devnull);
            }
        }
    }

    /* 写 PID 文件 */
    if (pidfile != NULL) {
        FILE *pf = fopen(pidfile, "w");
        if (pf != NULL) {
            fprintf(pf, "%d\n", getpid());
            fclose(pf);
        } else {
            fprintf(stderr, "daemon: cannot write pidfile %s: %s\n",
                    pidfile, strerror(errno));
        }
    }
}

int main(int argc, char *argv[])
{
    const char *server_ip;
    int server_port;
    const char *client_id;
    int retry_delay = 1;
    int do_daemon = 0;
    const char *pidfile = NULL;
    const char *logfile = NULL;
    int opt;

    /* 解析选项 */
    while ((opt = getopt(argc, argv, "dp:l:h")) != -1) {
        switch (opt) {
        case 'd':
            do_daemon = 1;
            break;
        case 'p':
            pidfile = optarg;
            break;
        case 'l':
            logfile = optarg;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }

    /* 剩余参数：server_ip server_port client_id */
    if (argc - optind != 3) {
        usage(argv[0]);
        return 1;
    }

    server_ip   = argv[optind];
    server_port = atoi(argv[optind + 1]);
    client_id   = argv[optind + 2];

    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }
    if (strlen(client_id) == 0 || strlen(client_id) >= MAX_CLIENT_ID) {
        fprintf(stderr, "Invalid client_id (max %d chars)\n", MAX_CLIENT_ID - 1);
        return 1;
    }

    /* 忽略 SIGPIPE。SIGCHLD 保持默认（不自动收割僵尸进程，waitpid 可用）。 */
    signal(SIGPIPE, SIG_IGN);

    /* 守护进程模式 */
    if (do_daemon) {
        daemonize(pidfile, logfile);
    } else if (pidfile != NULL) {
        /* 非守护模式也支持写 PID 文件 */
        FILE *pf = fopen(pidfile, "w");
        if (pf != NULL) {
            fprintf(pf, "%d\n", getpid());
            fclose(pf);
        }
    }

    /* 初始化工作目录 */
    char cwd[PATH_MAX];
    char new_cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "/");
    }

    printf("[client:%s] starting, connecting to %s:%d ...\n",
           client_id, server_ip, server_port);

    /* 主循环：连接 → 注册 → 收发命令 → 断线重连 */
    for (;;) {
        int fd;
        int registered = 0;

        fd = connect_server(server_ip, server_port);
        if (fd < 0) {
            fprintf(stderr, "[client:%s] connect failed: %s, retry in %d s\n",
                    client_id, strerror(errno), retry_delay);
            sleep(retry_delay);
            retry_delay = retry_delay * 2;
            if (retry_delay > MAX_RETRY_DELAY) {
                retry_delay = MAX_RETRY_DELAY;
            }
            continue;
        }

        /* 重连成功，重置退避 */
        retry_delay = 1;

        printf("[client:%s] connected to server (fd=%d)\n", client_id, fd);

        /* 发送 REGISTER */
        if (send_message(fd, MSG_REGISTER, 0, client_id,
                         (uint32_t)strlen(client_id) + 1) < 0) {
            fprintf(stderr, "[client:%s] send register failed\n", client_id);
            close(fd);
            sleep(retry_delay);
            continue;
        }

        /* 事件循环 */
        for (;;) {
            fd_set rset;
            struct timeval tv;
            int maxfd;
            int ret;

            FD_ZERO(&rset);
            FD_SET(fd, &rset);
            maxfd = fd;

            /* 如果有子进程在运行，也监听管道 */
            if (child_pipe >= 0) {
                FD_SET(child_pipe, &rset);
                if (child_pipe > maxfd) {
                    maxfd = child_pipe;
                }
            }

            tv.tv_sec = HEARTBEAT_INTERVAL;
            tv.tv_usec = 0;

            ret = select(maxfd + 1, &rset, NULL, NULL, &tv);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("[client] select");
                break;
            }

            /* 检查子进程是否已退出（非阻塞） */
            if (child_pid >= 0) {
                int ec;
                if (check_child_exit(&ec)) {
                    extract_cwd_from_output(new_cwd, sizeof(new_cwd));
                    if (new_cwd[0] != '\0') {
                        strncpy(cwd, new_cwd, sizeof(cwd) - 1);
                        cwd[sizeof(cwd) - 1] = '\0';
                    }
                    send_result(fd, cwd, ec, child_req_id, 0);
                    printf("[client:%s] command finished (req_id=%u, exit=%d, cwd=%s)\n",
                           client_id, child_req_id, ec, cwd);
                }
            }

            if (ret == 0) {
                /* 超时，发心跳 */
                if (send_message(fd, MSG_HEARTBEAT, 0, NULL, 0) < 0) {
                    fprintf(stderr, "[client:%s] send heartbeat failed\n",
                            client_id);
                    break;
                }
                /* 同时检查一下子进程 */
                if (child_pid >= 0) {
                    int ec;
                    if (check_child_exit(&ec)) {
                        extract_cwd_from_output(new_cwd, sizeof(new_cwd));
                        if (new_cwd[0] != '\0') {
                            strncpy(cwd, new_cwd, sizeof(cwd) - 1);
                            cwd[sizeof(cwd) - 1] = '\0';
                        }
                        send_result(fd, cwd, ec, child_req_id, 0);
                        printf("[client:%s] command finished (req_id=%u, exit=%d, cwd=%s)\n",
                               client_id, child_req_id, ec, cwd);
                    }
                }
                continue;
            }

            /* 子进程管道可读 */
            if (child_pipe >= 0 && FD_ISSET(child_pipe, &rset)) {
                read_child_output();

                /* 检查子进程是否已退出（读完数据后） */
                int ec;
                if (check_child_exit(&ec)) {
                    extract_cwd_from_output(new_cwd, sizeof(new_cwd));
                    if (new_cwd[0] != '\0') {
                        strncpy(cwd, new_cwd, sizeof(cwd) - 1);
                        cwd[sizeof(cwd) - 1] = '\0';
                    }
                    send_result(fd, cwd, ec, child_req_id, 0);
                    printf("[client:%s] command finished (req_id=%u, exit=%d, cwd=%s)\n",
                           client_id, child_req_id, ec, cwd);
                }
            }

            /* socket 可读 */
            if (FD_ISSET(fd, &rset)) {
                uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];
                msg_type_t type;
                uint32_t req_id;
                uint32_t payload_len;

                if (recv_message(fd, buf, sizeof(buf), &type, &req_id,
                                 NULL, &payload_len) < 0) {
                    fprintf(stderr, "[client:%s] connection lost\n", client_id);
                    break;
                }

                switch (type) {
                case MSG_REGISTER_ACK:
                    registered = 1;
                    printf("[client:%s] registered, waiting for commands...\n",
                           client_id);
                    break;

                case MSG_CMD: {
                    /* 如果已有命令在运行，忽略新命令 */
                    if (child_pid >= 0) {
                        fprintf(stderr, "[client:%s] command already running, "
                                "ignoring new CMD\n", client_id);
                        break;
                    }

                    /* 提取命令字符串（跳过 target_client_id\0） */
                    const char *cmd_str;
                    cmd_str = (const char *)memchr(buf + HEADER_SIZE, '\0',
                                                   payload_len);
                    if (cmd_str == NULL) {
                        cmd_str = (const char *)(buf + HEADER_SIZE);
                    } else {
                        cmd_str++;
                    }

                    printf("[client:%s] executing: %s\n", client_id, cmd_str);

                    /* 构造包装命令 */
                    char wrapped_cmd[MAX_CMD_LEN + PATH_MAX + 64];
                    snprintf(wrapped_cmd, sizeof(wrapped_cmd),
                             "cd '%s' && %s 2>&1 ; "
                             "echo \"__HX_CWD__:$(pwd)\"", cwd, cmd_str);

                    if (start_child(wrapped_cmd) == 0) {
                        child_req_id = req_id;
                    } else {
                        /* fork 失败，直接返回错误 */
                        char err_result[64];
                        snprintf(err_result, sizeof(err_result),
                                 "/\n-1\nfork failed: %s", strerror(errno));
                        send_message(fd, MSG_RESULT, req_id,
                                     err_result,
                                     (uint32_t)strlen(err_result) + 1);
                    }
                    break;
                }

                case MSG_CANCEL:
                    /* 取消正在运行的子进程 */
                    if (child_pid >= 0) {
                        printf("[client:%s] cancelling command (req_id=%u)\n",
                               client_id, child_req_id);

                        /* 先读走剩余输出 */
                        read_child_output();

                        /* 杀子进程 */
                        kill_child();

                        /* 尝试从已收集的输出中提取 CWD */
                        extract_cwd_from_output(new_cwd, sizeof(new_cwd));
                        if (new_cwd[0] != '\0') {
                            strncpy(cwd, new_cwd, sizeof(cwd) - 1);
                            cwd[sizeof(cwd) - 1] = '\0';
                        }

                        /* 发送被取消的结果 */
                        send_result(fd, cwd, -1, child_req_id, 1);
                        printf("[client:%s] command cancelled (req_id=%u)\n",
                               client_id, child_req_id);
                    }
                    break;

                case MSG_FILE_UPLOAD: {
                    /* 接收上传分块：payload = target_client_id\0 remote_path\0 file_meta_t data */
                    const uint8_t *p = buf + HEADER_SIZE;
                    uint32_t plen = payload_len;
                    const uint8_t *after_id;
                    const uint8_t *path_end;
                    const uint8_t *meta;
                    file_meta_t fm;
                    const uint8_t *data;
                    uint32_t data_len;
                    char remote_path[PATH_MAX];
                    size_t path_len;

                    after_id = (const uint8_t *)memchr(p, '\0', plen);
                    if (after_id == NULL) {
                        break;
                    }
                    after_id++;

                    /* 解析 remote_path（到下一个 \0） */
                    path_end = (const uint8_t *)memchr(after_id, '\0',
                        plen - (uint32_t)(after_id - p));
                    if (path_end == NULL) {
                        break;
                    }
                    path_len = (size_t)(path_end - after_id);
                    if (path_len == 0 || path_len >= PATH_MAX) {
                        break;
                    }
                    memcpy(remote_path, after_id, path_len);
                    remote_path[path_len] = '\0';

                    /* 解析 file_meta_t 与数据 */
                    meta = path_end + 1;
                    if ((uint32_t)(meta - p) + FILE_META_SIZE > plen) {
                        break;
                    }
                    file_meta_unpack(meta, &fm);
                    data = meta + FILE_META_SIZE;
                    data_len = plen - (uint32_t)(data - p);

                    /* 第一块：打开目标文件 */
                    if (fm.offset == 0) {
                        if (upload_fd >= 0) {
                            close(upload_fd);
                            upload_fd = -1;
                        }
                        upload_fd = open(remote_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                        if (upload_fd < 0) {
                            send_file_ack(fd, req_id, -1, strerror(errno));
                            printf("[client:%s] upload open failed: %s (%s)\n",
                                   client_id, remote_path, strerror(errno));
                            break;
                        }
                    }

                    if (upload_fd < 0) {
                        /* 句柄丢失（非第一块但未打开），报错 */
                        send_file_ack(fd, req_id, -1, "upload not open");
                        break;
                    }

                    /* 定位并写入 */
                    if (lseek(upload_fd, fm.offset, SEEK_SET) < 0 ||
                        write_full(upload_fd, data, data_len) != 0) {
                        send_file_ack(fd, req_id, -1, strerror(errno));
                        close(upload_fd);
                        upload_fd = -1;
                        break;
                    }

                    /* 每块都回 ACK（供管理端做流控），最后一块关闭文件 */
                    if (fm.flags & FILE_FLAG_FINAL) {
                        close(upload_fd);
                        upload_fd = -1;
                    }
                    send_file_ack(fd, req_id, 0, NULL);
                    if (fm.flags & FILE_FLAG_FINAL) {
                        printf("[client:%s] upload complete: %s (%u bytes)\n",
                               client_id, remote_path, fm.total_size);
                    }
                    break;
                }

                case MSG_FILE_DOWNLOAD: {
                    /* 下载请求：payload = target_client_id\0 remote_path\0 */
                    const uint8_t *p = buf + HEADER_SIZE;
                    uint32_t plen = payload_len;
                    const uint8_t *after_id;
                    const uint8_t *pend = p + plen;
                    const uint8_t *path_end;
                    char remote_path[PATH_MAX];
                    size_t path_len;
                    int in_fd;
                    struct stat st;
                    uint32_t total;
                    uint32_t off = 0;
                    uint8_t chunk[FILE_META_SIZE + FILE_CHUNK_SIZE];

                    after_id = (const uint8_t *)memchr(p, '\0', plen);
                    if (after_id == NULL) {
                        break;
                    }
                    after_id++;

                    path_end = (const uint8_t *)memchr(after_id, '\0',
                        (size_t)(pend - after_id));
                    if (path_end == NULL) {
                        path_len = (size_t)(pend - after_id);
                    } else {
                        path_len = (size_t)(path_end - after_id);
                    }
                    if (path_len == 0 || path_len >= PATH_MAX) {
                        break;
                    }
                    memcpy(remote_path, after_id, path_len);
                    remote_path[path_len] = '\0';

                    in_fd = open(remote_path, O_RDONLY);
                    if (in_fd < 0) {
                        send_file_ack(fd, req_id, -1, strerror(errno));
                        printf("[client:%s] download open failed: %s (%s)\n",
                               client_id, remote_path, strerror(errno));
                        break;
                    }

                    if (fstat(in_fd, &st) < 0) {
                        send_file_ack(fd, req_id, -1, strerror(errno));
                        close(in_fd);
                        break;
                    }
                    total = (uint32_t)st.st_size;

                    /* 流式读文件并分块回传 */
                    for (;;) {
                        ssize_t n = read(in_fd, chunk + FILE_META_SIZE, FILE_CHUNK_SIZE);
                        if (n < 0) {
                            send_file_ack(fd, req_id, -1, strerror(errno));
                            break;
                        }
                        if (n == 0) {
                            /* 空文件：发一个 FINAL 空块 */
                            if (total == 0) {
                                file_meta_t em = { 0, 0, FILE_FLAG_FINAL };
                                file_meta_pack(chunk, &em);
                                send_message(fd, MSG_FILE_DATA, req_id, chunk,
                                             FILE_META_SIZE);
                            }
                            break;
                        }

                        off += (uint32_t)n;
                        {
                            file_meta_t fm;
                            fm.offset = off - (uint32_t)n;
                            fm.total_size = total;
                            fm.flags = (off >= total) ? FILE_FLAG_FINAL : 0;
                            file_meta_pack(chunk, &fm);
                        }

                        if (send_message(fd, MSG_FILE_DATA, req_id, chunk,
                                         (uint32_t)(FILE_META_SIZE + n)) < 0) {
                            send_file_ack(fd, req_id, -1, strerror(errno));
                            break;
                        }

                        if (off >= total) {
                            break;
                        }
                    }

                    close(in_fd);
                    printf("[client:%s] download sent: %s (%u bytes)\n",
                           client_id, remote_path, total);
                    break;
                }

                case MSG_HEARTBEAT_ACK:
                    /* 心跳回复，忽略 */
                    break;

                default:
                    fprintf(stderr, "[client:%s] unknown msg type %d\n",
                            client_id, type);
                    break;
                }
            }
        }

        /* 连接断开，清理子进程 */
        kill_child();
        output_buf_reset();
        if (upload_fd >= 0) {
            close(upload_fd);
            upload_fd = -1;
        }

        close(fd);
        if (registered) {
            printf("[client:%s] disconnected, reconnecting in %d s...\n",
                   client_id, retry_delay);
        }
        sleep(retry_delay);
        retry_delay = retry_delay * 2;
        if (retry_delay > MAX_RETRY_DELAY) {
            retry_delay = MAX_RETRY_DELAY;
        }
    }

    return 0;
}

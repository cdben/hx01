/*
 * admin.c - 远程管理端（交互式控制台）
 *
 * 职责：
 *   1. 连接服务端
 *   2. 获取客户端列表，列表式选择目标客户端
 *   3. 进入类 Linux shell 模式，支持 cd、执行命令等
 *   4. 实时显示 [client_id:当前目录] 提示符
 *
 * 用法: ./admin <server_ip> <server_port>
 * 示例: ./admin 127.0.0.1 8888
 *
 * 交互:
 *   列表界面: 输入序号或 client_id 选择客户端，:quit 退出
 *   shell 界面: 输入命令执行，:quit 返回列表
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "protocol.h"
#include "utils.h"

#define MAX_CLIENT_LIST 256

/* 连接到服务端，返回 fd 或 -1 */
static int connect_server(const char *ip, int port)
{
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

/*
 * 从服务端接收一条消息，返回消息类型。
 * 只关心 LIST_RESP / RESULT / ERROR，其他类型忽略并继续读。
 */
static int recv_response(int fd, msg_type_t *type, uint32_t *req_id,
                         uint8_t *payload_buf, size_t payload_buf_size,
                         uint32_t *payload_len)
{
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];

    for (;;) {
        msg_type_t t;
        uint32_t rid;
        uint32_t plen;

        if (recv_message(fd, buf, sizeof(buf), &t, &rid, NULL, &plen) < 0) {
            return -1;
        }

        if (t == MSG_LIST_RESP || t == MSG_RESULT || t == MSG_ERROR) {
            *type = t;
            *req_id = rid;
            if (payload_buf != NULL && plen > 0) {
                size_t copy_len = plen < payload_buf_size ? plen : payload_buf_size;
                memcpy(payload_buf, buf + HEADER_SIZE, copy_len);
                if (copy_len < payload_buf_size) {
                    payload_buf[copy_len] = '\0';
                }
            }
            *payload_len = plen;
            return 0;
        }

        /* 忽略其他消息（如 HEARTBEAT_ACK 等） */
    }
}

/*
 * 请求客户端列表，返回解析后的客户端 ID 数组（calloc 分配）。
 * count 写出客户端数量。调用方负责 free。
 */
static char **fetch_client_list(int fd, int *count)
{
    uint8_t payload[MAX_PAYLOAD];
    msg_type_t type;
    uint32_t req_id;
    uint32_t payload_len;
    char **list = NULL;
    int n = 0;

    *count = 0;

    /* 发送 LIST 请求 */
    if (send_message(fd, MSG_LIST, 0, NULL, 0) < 0) {
        fprintf(stderr, "[admin] send LIST failed\n");
        return NULL;
    }

    /* 接收 LIST_RESP */
    if (recv_response(fd, &type, &req_id, payload, sizeof(payload),
                      &payload_len) < 0) {
        fprintf(stderr, "[admin] recv LIST_RESP failed\n");
        return NULL;
    }

    if (type != MSG_LIST_RESP) {
        fprintf(stderr, "[admin] expected LIST_RESP, got %s\n",
                msg_type_str(type));
        return NULL;
    }

    if (payload_len == 0) {
        /* 没有客户端在线 */
        return NULL;
    }

    /* 解析：payload 是 "\n" 分隔的 client_id 列表 */
    list = calloc(MAX_CLIENT_LIST, sizeof(char *));
    if (list == NULL) {
        return NULL;
    }

    char *saveptr;
    char *token = strtok_r((char *)payload, "\n", &saveptr);
    while (token != NULL && n < MAX_CLIENT_LIST) {
        list[n] = strdup(token);
        if (list[n] == NULL) {
            break;
        }
        n++;
        token = strtok_r(NULL, "\n", &saveptr);
    }

    *count = n;
    return list;
}

static void free_client_list(char **list, int count)
{
    if (list == NULL) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
}

/*
 * 显示客户端列表并让用户选择。
 * 返回选中的 client_id（strdup 分配），调用方 free。
 * quit 参数：如果用户输入 :quit，*quit 设为 1 并返回 NULL。
 * 其他情况返回 NULL 时 *quit = 0（无效选择或 EOF）。
 */
static char *select_client(char **list, int count, int *quit)
{
    *quit = 0;

    printf("\n");
    printf("========================================\n");
    printf("  Connected Clients\n");
    printf("========================================\n");

    if (count == 0) {
        printf("  (no clients connected)\n");
        printf("========================================\n");
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        printf("  %d. %s\n", i + 1, list[i]);
    }
    printf("========================================\n");
    printf("Select client (number, client_id, or :quit): ");

    char input[MAX_CLIENT_ID + 16];
    if (fgets(input, sizeof(input), stdin) == NULL) {
        *quit = 1;  /* EOF 也视为退出 */
        return NULL;
    }
    input[strcspn(input, "\n")] = '\0';

    if (strlen(input) == 0) {
        return NULL;
    }

    if (strcmp(input, ":quit") == 0 || strcmp(input, ":q") == 0) {
        *quit = 1;
        return NULL;
    }

    /* 尝试按序号匹配 */
    char *endptr;
    long num = strtol(input, &endptr, 10);
    if (*endptr == '\0' && num >= 1 && num <= count) {
        return strdup(list[num - 1]);
    }

    /* 尝试按 client_id 匹配 */
    for (int i = 0; i < count; i++) {
        if (strcmp(input, list[i]) == 0) {
            return strdup(list[i]);
        }
    }

    printf("Invalid selection: %s\n", input);
    return NULL;
}

static void print_shell_help(void)
{
    printf("Shell commands:\n");
    printf("  :quit, :q    Return to client list\n");
    printf("  :help, :h    Show this help\n");
    printf("  :list, :l    Refresh and show client list\n");
    printf("  <cmd>        Execute command on the selected client\n");
}

/*
 * 解析 RESULT payload。
 * 格式: cwd\nexit_code\n<output>
 * 提取 cwd（写入 cwd_buf），打印 output（跳过 exit_code 行，exit_code
 * 为 0 时不显示）。末尾自动补换行。
 * 返回 0 成功，-1 失败。
 */
static int parse_and_display_result(const uint8_t *payload, uint32_t payload_len,
                                     char *cwd_buf, size_t cwd_buf_size)
{
    const char *p = (const char *)payload;

    /* 提取第一行: cwd */
    const char *nl1 = memchr(p, '\n', payload_len);
    if (nl1 == NULL) {
        return -1;
    }

    size_t cwd_len = (size_t)(nl1 - p);
    if (cwd_len >= cwd_buf_size) {
        cwd_len = cwd_buf_size - 1;
    }
    memcpy(cwd_buf, p, cwd_len);
    cwd_buf[cwd_len] = '\0';

    /* 跳过第二行 exit_code */
    const char *after_exit = nl1 + 1;
    const char *nl2 = memchr(after_exit, '\n',
                             payload_len - (uint32_t)(after_exit - p));
    if (nl2 == NULL) {
        return 0;  /* 无实际输出 */
    }

    /* exit_code 非 0 时打印 */
    long ec = strtol(after_exit, NULL, 10);
    if (ec != 0) {
        printf("[exit: %ld]\n", ec);
    }

    /* 打印实际输出（第三行起），跳过末尾 \0 */
    const char *output = nl2 + 1;
    ptrdiff_t out_len = (p + payload_len) - output;
    if (out_len > 1) {
        out_len--;  /* 去掉 payload 末尾的 \0 */
        if (out_len > 0) {
            printf("%.*s", (int)out_len, output);
            if (output[out_len - 1] != '\n') {
                printf("\n");
            }
        }
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <server_ip> <server_port>\n", prog);
    fprintf(stderr, "Example: %s 127.0.0.1 8888\n", prog);
}

int main(int argc, char *argv[])
{
    const char *server_ip;
    int server_port;
    int fd;
    uint32_t req_id = 1;
    char cwd[PATH_MAX];

    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    server_ip = argv[1];
    server_port = atoi(argv[2]);
    if (server_port <= 0 || server_port > 65535) {
        fprintf(stderr, "Invalid port: %s\n", argv[2]);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    printf("[admin] connecting to %s:%d ...\n", server_ip, server_port);

    fd = connect_server(server_ip, server_port);
    if (fd < 0) {
        fprintf(stderr, "[admin] connect failed: %s\n", strerror(errno));
        return 1;
    }

    printf("[admin] connected to server\n");

    /* 主循环：列表选择 → shell 模式 */
    for (;;) {
        /* 获取客户端列表 */
        int client_count = 0;
        char **client_list = fetch_client_list(fd, &client_count);

        if (client_list == NULL && client_count == 0) {
            printf("\n[admin] no clients connected. ");
            printf("Waiting for clients... (type 'r' to refresh, 'q' to quit): ");

            char input[16];
            if (fgets(input, sizeof(input), stdin) == NULL) {
                break;
            }
            input[strcspn(input, "\n")] = '\0';
            if (input[0] == 'q' || input[0] == 'Q') {
                break;
            }
            /* 'r' or anything else: refresh */
            continue;
        }

        if (client_list == NULL) {
            fprintf(stderr, "[admin] connection to server lost\n");
            break;
        }

        /* 选择客户端 */
        char *selected = NULL;
        int quit = 0;

retry_selection:
        selected = select_client(client_list, client_count, &quit);
        if (quit) {
            free_client_list(client_list, client_count);
            break;  /* 用户选择退出 */
        }
        if (selected == NULL) {
            printf("\n");
            free_client_list(client_list, client_count);

            /* 重新获取列表 */
            client_list = fetch_client_list(fd, &client_count);
            if (client_list == NULL) {
                if (client_count == 0) {
                    continue;  /* 回到外层循环 */
                }
                fprintf(stderr, "[admin] connection to server lost\n");
                goto cleanup;
            }
            goto retry_selection;
        }

        /* 进入 shell 模式 */
        printf("\n");
        printf("========================================\n");
        printf("  Shell mode: %s\n", selected);
        printf("  Type :help for commands, :quit to go back\n");
        printf("========================================\n\n");

        /* 发送一个空操作命令，从回传结果中提取真实 CWD */
        {
            size_t id_len = strlen(selected) + 1;
            uint8_t init_payload[MAX_CLIENT_ID + 4];
            memcpy(init_payload, selected, id_len);
            memcpy(init_payload + id_len, ":", 2);  /* shell 空操作 */

            if (send_message(fd, MSG_CMD, req_id, init_payload,
                             (uint32_t)(id_len + 2)) == 0) {
                uint8_t resp_payload[MAX_PAYLOAD];
                msg_type_t resp_type;
                uint32_t resp_req_id;
                uint32_t resp_len;

                if (recv_response(fd, &resp_type, &resp_req_id,
                                  resp_payload, sizeof(resp_payload),
                                  &resp_len) == 0 &&
                    resp_type == MSG_RESULT) {
                    parse_and_display_result(resp_payload, resp_len,
                                              cwd, sizeof(cwd));
                } else {
                    strcpy(cwd, "?");
                }
            } else {
                strcpy(cwd, "?");
            }
            req_id++;
        }

        /* shell 循环 */
        for (;;) {
            char input[MAX_CMD_LEN + 32];

            printf("[%s:%s] $ ", selected, cwd);
            fflush(stdout);

            if (fgets(input, sizeof(input), stdin) == NULL) {
                /* EOF */
                printf("\n");
                break;
            }

            /* 去掉末尾换行 */
            input[strcspn(input, "\n")] = '\0';

            if (strlen(input) == 0) {
                continue;
            }

            /* 处理内置命令 */
            if (strcmp(input, ":quit") == 0 || strcmp(input, ":q") == 0) {
                break;
            }
            if (strcmp(input, ":help") == 0 || strcmp(input, ":h") == 0) {
                print_shell_help();
                continue;
            }
            if (strcmp(input, ":list") == 0 || strcmp(input, ":l") == 0) {
                /* 短暂回到列表视图 */
                free_client_list(client_list, client_count);
                client_list = fetch_client_list(fd, &client_count);
                if (client_list != NULL && client_count > 0) {
                    printf("\nConnected clients:\n");
                    for (int i = 0; i < client_count; i++) {
                        printf("  %d. %s\n", i + 1, client_list[i]);
                    }
                    printf("\n");
                } else {
                    printf("\n(no clients connected)\n\n");
                }
                continue;
            }

            /* 构造 payload: "client_id\0command" */
            size_t id_len = strlen(selected) + 1;  /* 含 \0 */
            size_t cmd_len = strlen(input) + 1;
            uint8_t payload[MAX_CLIENT_ID + MAX_CMD_LEN];

            if (id_len + cmd_len > sizeof(payload)) {
                printf("[admin] command too long\n");
                continue;
            }

            memcpy(payload, selected, id_len);
            memcpy(payload + id_len, input, cmd_len);
            uint32_t payload_len = (uint32_t)(id_len + cmd_len);

            /* 发送 CMD */
            if (send_message(fd, MSG_CMD, req_id, payload, payload_len) < 0) {
                fprintf(stderr, "[admin] send CMD failed: %s\n", strerror(errno));
                printf("[admin] connection lost\n");
                goto shell_break;
            }

            /* 等待响应（支持 Enter 取消） */
            printf("[running... Enter to cancel]\n");
            {
                uint8_t resp_payload[MAX_PAYLOAD];
                msg_type_t resp_type = MSG_ERROR;
                uint32_t resp_req_id = 0;
                uint32_t resp_len = 0;
                int got_response = 0;

                for (;;) {
                    fd_set rset;
                    FD_ZERO(&rset);
                    FD_SET(fd, &rset);
                    FD_SET(STDIN_FILENO, &rset);
                    int maxfd = fd > STDIN_FILENO ? fd : STDIN_FILENO;

                    int sr = select(maxfd + 1, &rset, NULL, NULL, NULL);
                    if (sr < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }

                    /* 结果先到 */
                    if (FD_ISSET(fd, &rset)) {
                        if (recv_response(fd, &resp_type, &resp_req_id,
                                          resp_payload, sizeof(resp_payload),
                                          &resp_len) == 0) {
                            got_response = 1;
                        }
                        break;
                    }

                    /* 用户按下 Enter → 取消 */
                    if (FD_ISSET(STDIN_FILENO, &rset)) {
                        char cancel_buf[16];
                        fgets(cancel_buf, sizeof(cancel_buf), stdin);

                        /* 发送 CANCEL */
                        size_t cid_len = strlen(selected) + 1;
                        uint8_t cancel_payload[MAX_CLIENT_ID];
                        memcpy(cancel_payload, selected, cid_len);
                        send_message(fd, MSG_CANCEL, req_id,
                                     cancel_payload, (uint32_t)cid_len);

                        printf("[admin] cancelling...\n");

                        /* 等待被取消的结果 */
                        if (recv_response(fd, &resp_type, &resp_req_id,
                                          resp_payload, sizeof(resp_payload),
                                          &resp_len) == 0) {
                            got_response = 1;
                        }
                        break;
                    }
                }

                if (!got_response) {
                    fprintf(stderr, "[admin] recv response failed\n");
                    goto shell_break;
                }

                if (resp_type == MSG_ERROR) {
                    printf("[admin] ERROR: %s\n", resp_payload);
                } else if (resp_type == MSG_RESULT) {
                    char new_cwd[PATH_MAX];
                    if (parse_and_display_result(resp_payload, resp_len,
                                                  new_cwd, sizeof(new_cwd)) == 0) {
                        strncpy(cwd, new_cwd, sizeof(cwd) - 1);
                        cwd[sizeof(cwd) - 1] = '\0';
                    } else {
                        printf("[admin] (malformed result)\n");
                    }
                } else {
                    printf("[admin] unexpected response type: %s\n",
                           msg_type_str(resp_type));
                }
            }

            req_id++;
        }

shell_break:
        free(selected);
        free_client_list(client_list, client_count);
    }

cleanup:
    close(fd);
    printf("[admin] exiting\n");
    return 0;
}

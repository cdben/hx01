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
#include "history.h"
#include "term.h"

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
 * 接收文件传输相关的应答，仅当消息类型属于文件传输且 req_id 匹配时返回。
 * 其余消息（其他 req_id、HEARTBEAT_ACK 等）跳过。
 */
static int recv_file_response(int fd, uint32_t expected_req_id,
                              msg_type_t *type, uint8_t *payload_buf,
                              size_t payload_buf_size, uint32_t *payload_len)
{
    uint8_t buf[HEADER_SIZE + MAX_PAYLOAD];

    for (;;) {
        msg_type_t t;
        uint32_t rid;
        uint32_t plen;

        if (recv_message(fd, buf, sizeof(buf), &t, &rid, NULL, &plen) < 0) {
            return -1;
        }

        if ((t == MSG_FILE_DATA || t == MSG_FILE_ACK || t == MSG_ERROR) &&
            rid == expected_req_id) {
            *type = t;
            if (payload_buf != NULL && plen > 0) {
                size_t copy_len = plen < payload_buf_size ? plen : payload_buf_size;
                memcpy(payload_buf, buf + HEADER_SIZE, copy_len);
            }
            *payload_len = plen;
            return 0;
        }
        /* 忽略无关消息 */
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
 * 带行编辑的输入读取：进入 raw 模式，逐字节读按键，
 * 支持 ←/→ 移动光标、Backspace/Delete、↑/↓ 浏览历史。
 * 回车确认。返回值写入 buf（以 \0 结尾），返回 0 成功、-1 EOF。
 * prompt 为提示符字符串，由本函数打印并参与整行重绘。
 */
static int read_line(char *buf, size_t size, history_t *hist, const char *prompt)
{
    size_t len = 0;       /* 已输入字符数 */
    size_t pos = 0;       /* 光标位置（0..len） */
    int c;

    if (size == 0 || prompt == NULL) {
        return 0;
    }
    printf("%s", prompt);   /* 打印提示符 */
    fflush(stdout);

    hist_reset_cursor(hist);

    for (;;) {
        fflush(stdout);
        c = getchar();

        if (c == EOF) {
            return -1;
        }

        if (c == '\r' || c == '\n') {
            buf[len] = '\0';
            putchar('\n');
            return 0;
        }

        if (c == 0x01) {  /* Ctrl+A: 行首 */
            while (pos > 0) { putchar('\b'); pos--; }
            continue;
        }
        if (c == 0x05) {  /* Ctrl+E: 行尾 */
            while (pos < len) { putchar(buf[pos]); pos++; }
            continue;
        }
        if (c == 0x0b) {  /* Ctrl+K: 删到行尾 */
            size_t old = len;
            len = pos;
            buf[len] = '\0';
            /* 用空格覆盖 pos..old，再回退到 pos */
            for (size_t i = pos; i < old; i++) { putchar(' '); }
            for (size_t i = pos; i < old; i++) { putchar('\b'); }
            continue;
        }
        if (c == 0x15) {  /* Ctrl+U: 删到行首 */
            size_t old = len;
            memmove(buf, buf + pos, len - pos);
            len -= pos;
            pos = 0;
            buf[len] = '\0';
            /* 重绘：提示符 + buf，光标回到 pos=0 */
            putchar('\r');
            printf("%s%s", prompt, buf);
            /* 清掉原 old 长度的残影 */
            for (size_t i = len; i < old; i++) { putchar(' '); }
            for (size_t i = len; i < old; i++) { putchar('\b'); }
            continue;
        }

        if (c == 127 || c == 0x08) {  /* Backspace */
            if (pos == 0) {
                continue;
            }
            memmove(buf + pos - 1, buf + pos, len - pos);
            len--;
            pos--;
            buf[len] = '\0';
            printf("\b%s ", buf + pos);
            for (size_t i = pos; i <= len; i++) {
                putchar('\b');
            }
            continue;
        }

        if (c == 0x1b) {  /* ESC: 可能是方向键 */
            int s1 = getchar();
            int s2 = getchar();
            if (s1 == EOF || s2 == EOF) {
                continue;
            }
            if (s1 == '[') {
                if (s2 == 'D') {  /* ← */
                    if (pos > 0) { putchar('\b'); pos--; }
                    continue;
                }
                if (s2 == 'C') {  /* → */
                    if (pos < len) { putchar(buf[pos]); pos++; }
                    continue;
                }
                if (s2 == 'A' || s2 == 'B') {  /* ↑/↓ */
                    const char *entry = (s2 == 'A')
                        ? hist_prev(hist) : hist_next(hist);
                    if (entry == NULL) {
                        if (s2 == 'B') {
                            entry = "";  /* 下到最新：清空 */
                        } else {
                            continue;
                        }
                    }
                    size_t old = len;
                    size_t elen = strlen(entry);
                    if (elen >= size) { elen = size - 1; }
                    memcpy(buf, entry, elen);
                    len = elen;
                    pos = len;
                    buf[len] = '\0';
                    /* 重绘：提示符 + 新历史条目，光标到行尾(pos=len) */
                    putchar('\r');
                    printf("%s%s", prompt, buf);
                    /* 清掉原 old 长度的残影（若历史条目更短） */
                    for (size_t i = len; i < old; i++) { putchar(' '); }
                    for (size_t i = len; i < old; i++) { putchar('\b'); }
                    continue;
                }
                if (s2 == 'H') {  /* Home */
                    while (pos > 0) { putchar('\b'); pos--; }
                    continue;
                }
                if (s2 == 'F') {  /* End */
                    while (pos < len) { putchar(buf[pos]); pos++; }
                    continue;
                }
            }
            continue;  /* 其他转义序列忽略 */
        }

        /* 普通可打印字符 */
        if (c >= 0x20 && c < 0x7f && len < size - 1) {
            memmove(buf + pos + 1, buf + pos, len - pos);
            buf[pos] = (char)c;
            len++;
            buf[len] = '\0';
            printf("%s", buf + pos);
            pos++;
            for (size_t i = pos; i < len; i++) { putchar('\b'); }
        }
    }
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
    printf("  :exit, :e    Exit the program\n");
    printf("  :help, :h    Show this help\n");
    printf("  :list, :l    Refresh and show client list\n");
    printf("  :push <local> <remote>        Upload local file to client\n");
    printf("  :pull <remote> <local>        Download client file to local\n");
    printf("  <cmd>        Execute command on the selected client\n");
    printf("Line editing: Up/Down history, Left/Right move cursor,\n");
    printf("              Ctrl+A/E line head/end, Ctrl+K/U kill to end/start\n");
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

/*
 * 上传本地文件到客户端：分块发送，每块等待 ACK。
 * payload = client_id\0 remote_path\0 file_meta_t data
 */
static void do_upload(int fd, const char *client_id, uint32_t req_id,
                      const char *local_path, const char *remote_path)
{
    FILE *fp;
    long total_long;
    uint32_t total;
    size_t id_len = strlen(client_id) + 1;
    size_t path_len = strlen(remote_path) + 1;
    size_t prefix_len = id_len + path_len + FILE_META_SIZE;
    size_t chunk_cap;
    uint8_t payload[MAX_PAYLOAD];
    uint8_t *pmeta;
    uint8_t *pdata;
    uint32_t offset = 0;

    fp = fopen(local_path, "rb");
    if (fp == NULL) {
        printf("[upload] cannot open local file '%s': %s\n",
               local_path, strerror(errno));
        return;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        printf("[upload] cannot seek '%s': %s\n", local_path, strerror(errno));
        fclose(fp);
        return;
    }
    total_long = ftell(fp);
    if (total_long < 0) {
        total_long = 0;
    }
    total = (uint32_t)total_long;
    rewind(fp);

    /* 前缀过长（client_id + remote_path）导致放不下数据时直接报错 */
    if (prefix_len >= MAX_PAYLOAD) {
        printf("[upload] client_id/path too long\n");
        fclose(fp);
        return;
    }
    chunk_cap = MAX_PAYLOAD - prefix_len;
    if (chunk_cap > FILE_CHUNK_SIZE) {
        chunk_cap = FILE_CHUNK_SIZE;
    }

    /* 固定前缀：client_id\0 remote_path\0，随后是 file_meta_t 与数据 */
    memcpy(payload, client_id, id_len);
    memcpy(payload + id_len, remote_path, path_len);
    pmeta = payload + id_len + path_len;
    pdata = pmeta + FILE_META_SIZE;

    for (;;) {
        size_t n = fread(pdata, 1, chunk_cap, fp);
        file_meta_t fm;
        fm.offset = offset;
        fm.total_size = total;
        fm.flags = (offset + (uint32_t)n >= total) ? FILE_FLAG_FINAL : 0;
        file_meta_pack(pmeta, &fm);

        uint32_t plen = (uint32_t)(pdata - payload) + (uint32_t)n;
        if (send_message(fd, MSG_FILE_UPLOAD, req_id, payload, plen) < 0) {
            printf("[upload] send failed: %s\n", strerror(errno));
            fclose(fp);
            return;
        }
        offset += (uint32_t)n;

        /* 等待本块 ACK */
        {
            uint8_t resp[256];
            msg_type_t type;
            uint32_t rlen;

            if (recv_file_response(fd, req_id, &type, resp, sizeof(resp),
                                   &rlen) < 0) {
                printf("[upload] connection lost\n");
                fclose(fp);
                return;
            }

            if (type == MSG_ERROR) {
                printf("[upload] ERROR: %s\n", resp);
                fclose(fp);
                return;
            }

            if (type == MSG_FILE_ACK && rlen >= 4) {
                int32_t status_net;
                int status;
                memcpy(&status_net, resp, 4);
                status = (int)ntohl((uint32_t)status_net);
                if (status != 0) {
                    printf("[upload] failed: %s\n", resp + 4);
                    fclose(fp);
                    return;
                }
            }
        }

        printf("\r[upload] %u/%u bytes", offset, total);
        fflush(stdout);

        if (fm.flags & FILE_FLAG_FINAL) {
            break;
        }
    }

    fclose(fp);
    printf("\n[upload] done: %s -> %s (%u bytes)\n",
           local_path, remote_path, total);
}

/*
 * 从客户端下载文件：发送请求后循环接收分块写盘。
 */
static void do_download(int fd, const char *client_id, uint32_t req_id,
                        const char *remote_path, const char *local_path)
{
    size_t id_len = strlen(client_id) + 1;
    size_t path_len = strlen(remote_path) + 1;
    uint8_t req[MAX_CLIENT_ID + PATH_MAX];
    FILE *fp;
    uint32_t total = 0;
    uint32_t received = 0;
    uint8_t resp[FILE_META_SIZE + FILE_CHUNK_SIZE];
    int ok = 0;

    if (id_len + path_len > sizeof(req)) {
        printf("[download] path too long\n");
        return;
    }
    memcpy(req, client_id, id_len);
    memcpy(req + id_len, remote_path, path_len);

    if (send_message(fd, MSG_FILE_DOWNLOAD, req_id, req,
                     (uint32_t)(id_len + path_len)) < 0) {
        printf("[download] send failed: %s\n", strerror(errno));
        return;
    }

    fp = fopen(local_path, "wb");
    if (fp == NULL) {
        printf("[download] cannot open local file '%s': %s\n",
               local_path, strerror(errno));
        return;
    }

    for (;;) {
        msg_type_t type;
        uint32_t rlen;

        if (recv_file_response(fd, req_id, &type, resp, sizeof(resp),
                               &rlen) < 0) {
            printf("[download] connection lost\n");
            break;
        }

        if (type == MSG_ERROR) {
            printf("[download] ERROR: %s\n", resp);
            break;
        }

        if (type == MSG_FILE_ACK) {
            if (rlen >= 4) {
                int32_t status_net;
                int status;
                memcpy(&status_net, resp, 4);
                status = (int)ntohl((uint32_t)status_net);
                if (status != 0) {
                    printf("[download] failed: %s\n", resp + 4);
                }
            }
            break;
        }

        if (type == MSG_FILE_DATA) {
            file_meta_t fm;
            uint32_t data_len;

            if (rlen < FILE_META_SIZE) {
                printf("[download] malformed DATA chunk\n");
                break;
            }
            file_meta_unpack(resp, &fm);
            total = fm.total_size;
            data_len = rlen - FILE_META_SIZE;

            if (fseek(fp, fm.offset, SEEK_SET) != 0 ||
                fwrite(resp + FILE_META_SIZE, 1, data_len, fp) != data_len) {
                printf("[download] write failed: %s\n", strerror(errno));
                break;
            }
            received += data_len;

            printf("\r[download] %u/%u bytes", received, total);
            fflush(stdout);

            if (fm.flags & FILE_FLAG_FINAL) {
                ok = 1;
                break;
            }
        }
    }

    fclose(fp);

    if (ok) {
        printf("\n[download] done: %s -> %s (%u bytes)\n",
               remote_path, local_path, received);
    } else {
        printf("\n[download] failed, removing partial file '%s'\n", local_path);
        unlink(local_path);
    }
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

    history_t hist;
    hist_init(&hist);

    int exit_prog = 0;   /* :exit 直接退出整个程序 */

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
        if (term_raw_enter() < 0) {
            fprintf(stderr, "[admin] cannot enter raw mode: %s\n", strerror(errno));
            free(selected);
            free_client_list(client_list, client_count);
            continue;
        }
        for (;;) {
            char input[MAX_CMD_LEN + 32];
            char prompt[64 + PATH_MAX];

            snprintf(prompt, sizeof(prompt), "[%s:%s] $ ", selected, cwd);

            if (read_line(input, sizeof(input), &hist, prompt) < 0) {
                /* EOF */
                printf("\n");
                break;
            }

            if (strlen(input) == 0) {
                continue;
            }

            /* 处理内置命令 */
            if (strcmp(input, ":quit") == 0 || strcmp(input, ":q") == 0) {
                break;
            }
            if (strcmp(input, ":exit") == 0 || strcmp(input, ":e") == 0) {
                exit_prog = 1;
                break;
            }
            /* 记入历史（:quit/:exit 除外），供 ↑/↓ 回调 */
            hist_add(&hist, input);
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

            /* 文件上传: :push <local_file> <remote_path> */
            if (strncmp(input, ":push", 5) == 0 &&
                (input[5] == '\0' || input[5] == ' ')) {
                char *saveptr = NULL;
                char *local_path;
                char *remote_path;

                strtok_r(input, " \t", &saveptr);           /* 跳过 :push */
                local_path = strtok_r(NULL, " \t", &saveptr);
                remote_path = strtok_r(NULL, " \t", &saveptr);

                if (local_path == NULL || remote_path == NULL) {
                    printf("Usage: :push <local_file> <remote_path>\n");
                } else {
                    do_upload(fd, selected, req_id, local_path, remote_path);
                    req_id++;
                }
                continue;
            }

            /* 文件下载: :pull <remote_path> <local_file> */
            if (strncmp(input, ":pull", 5) == 0 &&
                (input[5] == '\0' || input[5] == ' ')) {
                char *saveptr = NULL;
                char *remote_path;
                char *local_path;

                strtok_r(input, " \t", &saveptr);           /* 跳过 :pull */
                remote_path = strtok_r(NULL, " \t", &saveptr);
                local_path = strtok_r(NULL, " \t", &saveptr);

                if (remote_path == NULL || local_path == NULL) {
                    printf("Usage: :pull <remote_path> <local_file>\n");
                } else {
                    do_download(fd, selected, req_id, remote_path, local_path);
                    req_id++;
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
                        /* raw 模式下逐字节读到回车/换行 */
                        int ch;
                        while ((ch = getchar()) != EOF && ch != '\n' && ch != '\r') {
                            /* 丢弃缓冲中的字符 */
                        }

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
        term_raw_exit();
        free(selected);
        free_client_list(client_list, client_count);
        if (exit_prog) {
            goto cleanup;
        }
    }

cleanup:
    close(fd);
    printf("[admin] exiting\n");
    return 0;
}

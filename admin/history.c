#include "history.h"

#include <string.h>

void hist_init(history_t *h)
{
    memset(h, 0, sizeof(*h));
    h->cursor = -1;
}

void hist_add(history_t *h, const char *cmd)
{
    size_t len;

    if (cmd == NULL) {
        return;
    }

    len = strlen(cmd);
    if (len == 0) {
        return;  /* 空命令不入库 */
    }

    /* 与最近一条相同则不重复添加 */
    if (h->count > 0) {
        int last = h->count - 1;
        if (strcmp(h->entries[last], cmd) == 0) {
            return;
        }
    }

    if (h->count < HIST_MAX) {
        strncpy(h->entries[h->count], cmd, HIST_BUF - 1);
        h->entries[h->count][HIST_BUF - 1] = '\0';
        h->count++;
    } else {
        /*
         * 满了则整体左移，丢弃最旧的一条，新条目放末尾。
         * （简单实现，命令数不多时性能足够。）
         */
        int i;
        for (i = 0; i < HIST_MAX - 1; i++) {
            memcpy(h->entries[i], h->entries[i + 1], HIST_BUF);
        }
        strncpy(h->entries[HIST_MAX - 1], cmd, HIST_BUF - 1);
        h->entries[HIST_MAX - 1][HIST_BUF - 1] = '\0';
    }

    h->cursor = -1;  /* 添加后重置游标 */
}

const char *hist_prev(history_t *h)
{
    if (h->count == 0) {
        return NULL;
    }

    if (h->cursor == -1) {
        /* 从最新位置开始向上 */
        h->cursor = h->count - 1;
    } else if (h->cursor > 0) {
        h->cursor--;
    } else {
        /* 已到最旧 */
        return NULL;
    }

    return h->entries[h->cursor];
}

const char *hist_next(history_t *h)
{
    if (h->cursor == -1) {
        return NULL;  /* 已在最新位置 */
    }

    h->cursor++;
    if (h->cursor >= h->count) {
        h->cursor = -1;
        return NULL;  /* 回到新输入位置 */
    }

    return h->entries[h->cursor];
}

void hist_reset_cursor(history_t *h)
{
    h->cursor = -1;
}

#ifndef HISTORY_H
#define HISTORY_H

#include <stddef.h>

#define HIST_MAX   100   /* 最多保存 100 条历史命令 */
#define HIST_BUF   1024  /* 每条命令最大长度 */

/*
 * 历史命令管理器。
 * 用数组实现环形缓冲，按下方向键上/下时遍历历史。
 */

/* 历史记录结构 */
typedef struct {
    char entries[HIST_MAX][HIST_BUF];  /* 存储历史命令 */
    int  count;    /* 已存命令数（不超过 HIST_MAX） */
    int  cursor;   /* 当前浏览位置，-1 表示在最新位置（新输入） */
} history_t;

/* 初始化历史记录 */
void hist_init(history_t *h);

/* 添加一条命令到历史。空命令不入库。 */
void hist_add(history_t *h, const char *cmd);

/*
 * 向上浏览历史（方向键上）。
 * 返回指向历史命令字符串的指针，或 NULL 表示没有更早的历史。
 */
const char *hist_prev(history_t *h);

/*
 * 向下浏览历史（方向键下）。
 * 返回指向历史命令字符串的指针，或 NULL 表示已回到最新位置。
 */
const char *hist_next(history_t *h);

/* 重置浏览游标到最新位置（开始输入新命令时调用）。 */
void hist_reset_cursor(history_t *h);

#endif /* HISTORY_H */

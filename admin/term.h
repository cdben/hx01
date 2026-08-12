#ifndef TERM_H
#define TERM_H

/*
 * 终端原始终端模式控制。
 * 进入 raw 模式后，终端不缓冲输入、不回显、不处理特殊字符，
 * 程序可以逐字节读取按键（包括方向键转义序列）。
 */

/* 进入 raw 模式，保存原始终端属性。成功返回 0，失败返回 -1。 */
int term_raw_enter(void);

/* 恢复原始终端属性。 */
void term_raw_exit(void);

#endif /* TERM_H */

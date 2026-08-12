#include "term.h"

#include <termios.h>
#include <unistd.h>

static struct termios orig_termios;
static int term_saved = 0;

int term_raw_enter(void)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) {
        return -1;
    }
    term_saved = 1;

    raw = orig_termios;
    /*
     * ~ICANON: 关闭行缓冲，逐字节读取
     * ~ECHO:   关闭回显（由程序自己控制显示）
     * ~ISIG:   不处理 Ctrl+C 等信号（我们自己处理）
     */
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;   /* 至少读 1 字节才返回 */
    raw.c_cc[VTIME] = 0;  /* 不超时 */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return -1;
    }

    return 0;
}

void term_raw_exit(void)
{
    if (term_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        term_saved = 0;
    }
}

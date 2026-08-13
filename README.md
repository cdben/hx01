# 远程管理 Linux 服务器 - 三端 Demo

## 架构

```
┌──────────┐   命令    ┌──────────┐   转发命令  ┌──────────┐
│  管理端   │ ────────→ │  服务端   │ ─────────→ │  客户端   │
│  admin   │           │  server  │            │  client  │
│          │ ←──────── │          │ ←───────── │          │
└──────────┘   结果    └──────────┘   回传结果  └──────────┘
                        (公网/中转)
```

- **客户端 (client)**：部署在被管理的服务器上，主动反向连接服务端，注册 ID，执行收到的命令并回传结果
- **服务端 (server)**：中转枢纽，管理客户端连接列表，转发管理端命令到指定客户端，转发结果回管理端
- **管理端 (admin)**：交互式控制台，向指定客户端发送命令，接收并显示结果

## 消息协议

```
+--------+--------+----------+----------+--------+----------------+
| magic  |  type  | reserved |  req_id  | length |    payload     |
| 2 byte | 1 byte |  1 byte  |  4 byte  | 4 byte |  length byte   |
+--------+--------+----------+----------+--------+----------------+

magic   = 0x5A53 (大端序)
type    = 1:REGISTER  2:REGISTER_ACK  3:CMD  4:RESULT  5:HEARTBEAT  6:HEARTBEAT_ACK  7:ERROR  8:LIST  9:LIST_RESP  10:CANCEL
req_id  = 请求ID（管理端生成，客户端原样回填，用于匹配命令和结果）
length  = payload 长度（用于粘包处理：先读满 12 字节帧头，再按 length 读 payload）
```

| type | 名称           | 方向                          | payload 说明 |
|------|----------------|-------------------------------|--------------|
| 1    | REGISTER       | client → server               | `client_id` 字符串（含 `\0`） |
| 2    | REGISTER_ACK   | server → client               | 空 |
| 3    | CMD            | admin → server → client       | `target_client_id\0command` |
| 4    | RESULT         | client → server → admin       | `cwd\nexit_code\n<output>` |
| 5    | HEARTBEAT      | client → server               | 空 |
| 6    | HEARTBEAT_ACK  | server → client               | 空 |
| 7    | ERROR          | server → admin                | 错误信息字符串（含 `\0`） |
| 8    | LIST           | admin → server                | 空，请求在线客户端列表 |
| 9    | LIST_RESP      | server → admin                | `id1\nid2\n...` 换行分隔的 client_id 列表 |
| 10   | CANCEL         | admin → server → client       | `target_client_id` 字符串（含 `\0`），服务端按 client_id 转发 |

其中 RESULT 的 payload 格式为 `cwd\nexit_code\n<output>`：第一行是命令执行后的工作目录（用于管理端实时显示提示符），第二行是退出码，其后是命令输出。客户端通过把命令包装成 `cd '<cwd>' && <cmd> ; echo "__HX_CWD__:$(pwd)"` 来追踪 `cd` 产生的目录变化。

## 快速开始

### 1. 编译

```bash
make          # 编译全部三个端
make clean    # 清理
```

### 2. 启动（需要开 3~4 个终端）

```bash
# 终端 1: 启动服务端
make run-server

# 终端 2: 启动客户端 web-01
make run-client1

# 终端 3: 启动客户端 web-02（测试多台服务器）
make run-client2

# 终端 4: 启动管理端
make run-admin
```

### 3. 在管理端操作

管理端启动后会先拉取在线客户端列表，输入序号或 client_id 选择目标，随后进入类 Linux shell 模式，提示符实时显示当前远端工作目录。在 shell 中输入命令即对所选客户端执行，支持 `cd` 等会改变工作目录的命令：

```
========================================
  Connected Clients
========================================
  1. web-01
  2. web-02
========================================
Select client (number, client_id, or :quit): 1
========================================
  Shell mode: web-01
========================================

[web-01:/home/user] $ uname -a
[web-01:/home/user] $ cd /tmp
[web-01:/tmp] $ ls -la
[web-01:/tmp] $ :push ./report.tar.gz /tmp/report.tar.gz
[web-01:/tmp] $ :pull /var/log/syslog ./syslog.log
[web-01:/tmp] $ :quit
```

- 输入序号 `1` 选择 web-01，进入 shell 模式
- 提示符 `[client_id:当前目录] $` 实时反映远端工作目录
- `cd` 会改变目录，效果持久化（客户端维护 `cwd`，每次命令从上次目录出发）
- 命令执行成功时不显示退出码，失败时显示 `[exit: N]`
- 长命令执行期间显示 `[running... Enter to cancel]`，按回车即可取消
- `:quit` 或 `:q` 返回客户端列表；`:exit` 或 `:e` 退出管理端程序

Shell 模式内置命令：

| 命令      | 说明 |
|-----------|------|
| `:quit` / `:q` | 返回客户端列表 |
| `:exit` / `:e` | 退出管理端程序 |
| `:help` / `:h` | 显示帮助 |
| `:list` / `:l` | 刷新并显示客户端列表 |
| `:push <local> <remote>` | 上传本地文件到客户端（原 `:upload`） |
| `:pull <remote> <local>` | 下载客户端文件到本地（原 `:download`） |
| 其他输入   | 作为 shell 命令在所选客户端上执行 |

### 4. 添加更多客户端

```bash
# 直接手动启动，指定不同的 client_id
./client/client 127.0.0.1 8888 db-01
./client/client 127.0.0.1 8888 cache-01

# 守护进程模式（后台运行）
./client/client -d -p /var/run/hx01.pid -l /var/log/hx01.log 127.0.0.1 8888 web-01
```

客户端支持以下选项：

| 选项 | 说明 |
|------|------|
| `-d` | 守护进程模式（fork 后台运行，脱离终端） |
| `-p <file>` | 写入 PID 文件 |
| `-l <file>` | stdout/stderr 重定向到日志文件 |
| `-h` | 显示帮助 |

守护进程模式会执行标准 double-fork：`fork` → `setsid` → `fork` → `chdir("/")` → 重定向 stdio。

启动后可通过 PID 文件管理客户端进程：

```bash
# 启动守护进程客户端
./client/client -d -p /var/run/hx01.pid -l /var/log/hx01.log 10.0.0.1 8888 web-01

# 查看运行状态
kill -0 $(cat /var/run/hx01.pid) && echo "running" || echo "stopped"

# 停止客户端
kill $(cat /var/run/hx01.pid)

# 查看实时日志
tail -f /var/log/hx01.log
```

> 停止时会触发客户端的断线重连逻辑——服务端和管理端会看到 client disconnected。配合 `systemd` 或 `launchd` 可实现崩溃自动重启。

### 5. 开机自启动

#### Linux（systemd）

创建 service 模板单元 `/etc/systemd/system/hx01-client@.service`：

```ini
[Unit]
Description=HX01 Remote Client (%i)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/hx01/client -d -p /var/run/hx01-%i.pid -l /var/log/hx01-%i.log <server_ip> <server_port> %i
ExecStop=/bin/kill $(cat /var/run/hx01-%i.pid)
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

`%i` 是实例占位符，部署时是一条命令：

```bash
# 部署可执行文件
cp client /opt/hx01/client

# 启用 web-01 实例（开机自启 + 立即启动）
systemctl enable --now hx01-client@web-01.service

# 多台机器
systemctl enable --now hx01-client@db-01.service
systemctl enable --now hx01-client@cache-01.service

# 查看状态 / 日志
systemctl status hx01-client@web-01.service
journalctl -u hx01-client@web-01.service -f
```

#### macOS（launchd）

创建 plist 文件 `~/Library/LaunchAgents/com.hx01.client.plist`：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.hx01.client</string>
    <key>ProgramArguments</key>
    <array>
        <string>/opt/hx01/client</string>
        <string>-d</string>
        <string>-p</string>
        <string>/tmp/hx01-client.pid</string>
        <string>-l</string>
        <string>/tmp/hx01-client.log</string>
        <string>127.0.0.1</string>
        <string>8888</string>
        <string>mac-web-01</string>
    </array>
    <key>KeepAlive</key>
    <true/>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/tmp/hx01-client.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/hx01-client.log</string>
</dict>
</plist>
```

```bash
# 加载（开机自启 + 立即启动）
launchctl load ~/Library/LaunchAgents/com.hx01.client.plist

# 卸载
launchctl unload ~/Library/LaunchAgents/com.hx01.client.plist

# 查看状态
launchctl list | grep hx01
```

> **说明**：客户端自带断线重连（指数退避），`Restart=always` 或 `KeepAlive` 提供第二层保障——即使进程崩溃也会被系统重新拉起。两层保护叠加，确保客户端始终在线。

## Makefile 命令速查

顶部变量统一管理连接参数，修改一处全局生效：

```makefile
SERVER_HOST = 127.0.0.1   # 服务端 IP
SERVER_PORT = 8888         # 服务端端口
CLIENT_ID1  = web-01       # 预设客户端 ID
CLIENT_ID2  = web-02
CLIENT_ID3  = db-01
DAEMON_PID_DIR = /tmp      # 守护进程 PID 文件目录
DAEMON_LOG_DIR = /tmp      # 守护进程日志目录
```

| 命令 | 说明 |
|------|------|
| `make` | 编译全部 |
| `make clean` | 清理编译产物 |
| `make run-server` | 启动服务端 |
| `make run-client1` | 客户端 web-01（前台） |
| `make run-client1d` | 客户端 web-01（守护进程） |
| `make run-client2` | 客户端 web-02（前台） |
| `make run-client2d` | 客户端 web-02（守护进程） |
| `make run-client3` | 客户端 db-01（前台） |
| `make run-client3d` | 客户端 db-01（守护进程） |
| `make run-admin` | 启动管理端 |
| `make run-all` | 一键启动 server + 2 client + admin（开发调试） |

## 目录结构

```
.
├── Makefile           # 编译和运行命令
├── README.md          # 本文件
├── common/            # 公共协议层
│   ├── protocol.h     # 消息帧定义、常量、枚举
│   ├── protocol.c     # 消息打包/解析
│   ├── utils.h        # 网络辅助函数声明
│   └── utils.c        # send_message/recv_message 等
├── server/            # 服务端
│   └── server.c       # select 事件循环，中转转发
├── client/            # 客户端（被控端）
│   └── client.c       # 反向连接、注册、fork/exec 执行命令（CWD 追踪，支持取消）、心跳、重连
└── admin/             # 管理端
    ├── admin.c        # 交互式控制台（列表选择 + shell 模式 + 行编辑输入）
    ├── history.h      # 命令历史管理（环形缓冲，方向键浏览）
    ├── history.c      # 历史记录实现
    ├── term.h         # 终端 raw 模式控制声明
    └── term.c         # termios raw 模式实现
```

> `admin/history.*` 提供命令历史环形缓冲，`admin/term.*` 提供终端 raw 模式；`admin.c` 中 `read_line()` 进入 raw 模式逐字节读取按键，接入 ↑/↓ 历史浏览与 ←/→ 光标移动、Backspace、Ctrl+A/E/K/U 行编辑。Makefile 已编译这两个模块（`admin/history.o`、`admin/term.o`）。

## 关键特性

- ✅ 客户端**反向连接**（不需要被控端开端口）
- ✅ 管理端**列表式选择**客户端（`:list` 刷新，支持序号或名称选择）
- ✅ **类 Shell 交互**（提示符实时显示 `[client_id:当前目录]`，`cd` 效果持久化）
- ✅ **命令取消**（长命令执行期间按 Enter 即可中断，`MSG_CANCEL` → `SIGTERM`）
- ✅ **stderr 捕获**（`2>&1` 合并标准错误，cargo 等编译输出也能看到）
- ✅ 管理端控制 **N 台服务器**（通过 client_id 路由）
- ✅ **心跳保活**（15秒间隔，防 NAT 超时）
- ✅ **断线重连**（指数退避 1→2→4→...→60秒）
- ✅ **请求ID匹配**（异步，管理端可连续发多条命令）
- ✅ **粘包处理**（帧头含 length，按长度读取）
- ✅ **文件传输**（`:push` 上传 / `:pull` 下载，分块传输 + 进度显示）
- ✅ **行编辑与历史**（↑/↓ 浏览历史命令，←/→ 移动光标，Backspace/Ctrl+A/E/K/U 行编辑）
- ✅ 跨平台（select 模型，macOS/Linux 通用）

## 后续改进方向

- [ ] TLS 加密（mbedtls / openssl）
- [ ] 客户端鉴权（预共享密钥或证书）
- [ ] 管理端登录鉴权
- [x] 客户端列表查询（`MSG_LIST` / `MSG_LIST_RESP`，管理端列表选择 + `:list` 命令）
- [x] 命令历史与方向键浏览（↑/↓ 回调历史，←/→ 移动光标，Backspace/Ctrl+A/E/K/U 编辑，基于 `admin/history.*`、`admin/term.*`）
- [ ] 命令审计日志
- [x] 文件传输功能（`:push` 上传 / `:pull` 下载，支持分块传输与进度显示）

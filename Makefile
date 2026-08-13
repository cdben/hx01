# ============================================================
#  远程管理 Linux 服务器 - 三端 Demo Makefile
#
#  目录结构:
#    common/  - 公共协议层 (protocol.c, utils.c)
#    server/  - 服务端 (中转枢纽)
#    client/  - 客户端 (被控端, 反向连接)
#    admin/   - 管理端 (交互式控制台)
#
#  快速开始:
#    make              # 编译全部
#    make run-server   # 启动服务端
#    make run-client1  # 启动客户端 web-01
#    make run-client2  # 启动客户端 web-02
#    make run-admin    # 启动管理端
#    make clean        # 清理
# ============================================================

# ---- 编译选项 ----
CC        = gcc
CFLAGS    = -Wall -Wextra -g -Icommon
LDFLAGS   =

# ---- 服务端配置 ----
SERVER_HOST = 127.0.0.1
SERVER_PORT = 8888

# ---- 客户端预设 ID ----
CLIENT_ID1  = web-01
CLIENT_ID2  = web-02
CLIENT_ID3  = db-01

# ---- 守护进程相关 ----
DAEMON_PID_DIR  = /tmp
DAEMON_LOG_DIR  = /tmp

# 公共目标文件
COMMON_OBJ = common/protocol.o common/utils.o

# 默认目标: 编译全部三个端
all: server/server client/client admin/admin

# --- 公共层 ---
common/protocol.o: common/protocol.c common/protocol.h
	$(CC) $(CFLAGS) -c $< -o $@

common/utils.o: common/utils.c common/utils.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- 服务端 ---
server/server: server/server.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) -o $@ $(LDFLAGS)

# --- 客户端 ---
client/client: client/client.c $(COMMON_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) -o $@ $(LDFLAGS)

# --- 管理端 ---
ADMIN_OBJ = admin/history.o admin/term.o

admin/history.o: admin/history.c admin/history.h
	$(CC) $(CFLAGS) -c $< -o $@

admin/term.o: admin/term.c admin/term.h
	$(CC) $(CFLAGS) -c $< -o $@

admin/admin: admin/admin.c $(COMMON_OBJ) $(ADMIN_OBJ)
	$(CC) $(CFLAGS) $< $(COMMON_OBJ) $(ADMIN_OBJ) -o $@ $(LDFLAGS)

# ============================================================
#  运行命令
# ============================================================

# --- 服务端 ---
run-server: server/server
	./server/server $(SERVER_PORT)

# --- 客户端（前台） ---
run-client1: client/client
	./client/client $(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID1)

run-client2: client/client
	./client/client $(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID2)

run-client3: client/client
	./client/client $(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID3)

# --- 客户端（守护进程） ---
run-client1d: client/client
	./client/client -d \
		-p $(DAEMON_PID_DIR)/hx01-$(CLIENT_ID1).pid \
		-l $(DAEMON_LOG_DIR)/hx01-$(CLIENT_ID1).log \
		$(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID1)

run-client2d: client/client
	./client/client -d \
		-p $(DAEMON_PID_DIR)/hx01-$(CLIENT_ID2).pid \
		-l $(DAEMON_LOG_DIR)/hx01-$(CLIENT_ID2).log \
		$(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID2)

run-client3d: client/client
	./client/client -d \
		-p $(DAEMON_PID_DIR)/hx01-$(CLIENT_ID3).pid \
		-l $(DAEMON_LOG_DIR)/hx01-$(CLIENT_ID3).log \
		$(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID3)

# --- 管理端 ---
run-admin: admin/admin
	./admin/admin $(SERVER_HOST) $(SERVER_PORT)

# --- 同时启动全部（开发调试） ---
run-all: all
	@echo "Starting server + 2 clients + admin ..."
	./server/server $(SERVER_PORT) &
	sleep 0.3
	./client/client $(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID1) &
	./client/client $(SERVER_HOST) $(SERVER_PORT) $(CLIENT_ID2) &
	sleep 0.5
	./admin/admin $(SERVER_HOST) $(SERVER_PORT)

# --- 清理 ---
clean:
	rm -f $(COMMON_OBJ) server/server client/client admin/admin
	rm -rf server/*.dSYM client/*.dSYM admin/*.dSYM

.PHONY: all clean run-all \
        run-server \
        run-client1 run-client2 run-client3 \
        run-client1d run-client2d run-client3d \
        run-admin

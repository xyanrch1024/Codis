#!/bin/bash
set -e

LOG_LEVEL="${LOG_LEVEL:-info}"
echo "=== Codis + Feishu Bot (log: ${LOG_LEVEL}) ==="

# 启动 Codis Server (后台)
echo "[1/2] Starting Codis server on port ${SERVER_PORT}..."
OPENCODE_LOG_LEVEL=${LOG_LEVEL} opencode-server -p ${SERVER_PORT} -c /app/config/config.toml &

# 等待 Server 就绪
for i in $(seq 1 30); do
    if curl -s http://127.0.0.1:${SERVER_PORT}/api/v1/health > /dev/null 2>&1; then
        echo "      Server ready"
        break
    fi
    sleep 1
done

# 启动 Feishu Bot (前台)
echo "[2/3] Starting Feishu bot..."
exec python3 /app/bot/feishu_bot.py

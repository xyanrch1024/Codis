#!/bin/bash
set -e

LOG_LEVEL="${LOG_LEVEL:-info}"
echo "=== Codis Server (log: ${LOG_LEVEL}) ==="

echo "Starting Codis server on port ${SERVER_PORT}..."
exec codis-server -p ${SERVER_PORT} -c /app/config/config.toml

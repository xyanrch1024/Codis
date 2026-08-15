#!/bin/bash
# Deploy codis.chat website to 43.131.33.40 (nginx static hosting)
# Usage: scripts/deploy.sh
set -euo pipefail

SERVER="ubuntu@43.131.33.40"
ROOT="/var/www/codis.chat"
DIST="website/dist"

if [ ! -d "$DIST" ]; then
  echo "building..."
  (cd website && npm run build)
fi

echo "== uploading dist/ =="
ssh "$SERVER" "sudo mkdir -p $ROOT && sudo chown -R \$USER $ROOT"
rsync -az --delete "$DIST/" "$SERVER:$ROOT/"

echo "== nginx config =="
ssh "$SERVER" 'cat > /tmp/codis-site.conf <<'"'"'EOF'"'"'
server {
    listen 80;
    server_name codis.chat;

    root /var/www/codis.chat;
    index index.html;

    location / {
        try_files $uri $uri/ =404;
    }

    # 静态资源缓存
    location ~* \.(css|js|svg|png|jpg|webp|woff2?)$ {
        expires 7d;
        add_header Cache-Control "public";
    }

    gzip on;
    gzip_types text/plain text/css application/javascript application/json image/svg+xml;

    access_log /var/log/nginx/codis.access.log;
    error_log  /var/log/nginx/codis.error.log;

    # HTTPS（证书就绪后取消注释，并把 80 改为跳转）
    # listen 443 ssl http2;
    # ssl_certificate     /etc/letsencrypt/live/codis.chat/fullchain.pem;
    # ssl_certificate_key /etc/letsencrypt/live/codis.chat/privkey.pem;
}
EOF
sudo cp /tmp/codis-site.conf /etc/nginx/conf.d/codis.conf
sudo nginx -t && sudo systemctl reload nginx'

echo "== done: http://codis.chat =="
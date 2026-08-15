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
# 一键安装脚本双份发布：/install 与 /install.sh（rsync --delete 后补）
ssh "$SERVER" "cp $ROOT/install.sh $ROOT/install && echo install-script-ready"

echo "== nginx config =="
ssh "$SERVER" 'cat > /tmp/codis-site.conf <<'"'"'EOF'"'"'
server {
    listen 80;
    server_name codis.chat;

    location /.well-known/acme-challenge/ { root /var/www/codis.chat; }

    location / { return 301 https://$host$request_uri; }
}

server {
    listen 443 ssl http2;
    server_name codis.chat;

    ssl_certificate     /etc/letsencrypt/live/codis.chat/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/codis.chat/privkey.pem;

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
}
EOF
sudo cp /tmp/codis-site.conf /etc/nginx/conf.d/codis.conf
sudo nginx -t && sudo systemctl reload nginx'

echo "== done: https://codis.chat =="
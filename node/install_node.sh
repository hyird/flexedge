#!/usr/bin/env bash
set -euo pipefail
umask 077

if [ "$(id -u)" -ne 0 ]; then
    echo "请使用 root 用户执行"
    exit 1
fi

: "${FLEXEDGE_SERVER_ORIGIN:?缺少 FLEXEDGE_SERVER_ORIGIN}"

if [ "$#" -ne 2 ]; then
    echo "用法: install-node.sh <node-id> <secret>"
    exit 1
fi
node_id="$1"
node_secret="$2"

case "$FLEXEDGE_SERVER_ORIGIN" in
    http://*|https://*) ;;
    *)
        echo "FLEXEDGE_SERVER_ORIGIN 必须以 http:// 或 https:// 开头"
        exit 1
        ;;
esac

websocket_origin="${FLEXEDGE_SERVER_ORIGIN/#http/ws}"
node_binary="$(mktemp /tmp/flexedge-node.XXXXXX)"
node_headers="$(mktemp /tmp/flexedge-node-headers.XXXXXX)"
node_credentials="$(mktemp /tmp/flexedge-credentials.XXXXXX)"

cleanup() {
    rm -f "$node_binary"
    rm -f "$node_headers"
    rm -f "$node_credentials"
}
trap cleanup EXIT

case "$node_id" in
    *[!0-9a-f]*|'')
        echo "node-id 必须是 32 位小写十六进制字符串"
        exit 1
        ;;
esac
if [ "${#node_id}" -ne 32 ]; then
    echo "node-id 必须是 32 位小写十六进制字符串"
    exit 1
fi
if [ "${#node_secret}" -lt 32 ] || [ "${#node_secret}" -gt 128 ]; then
    echo "secret 长度必须在 32 到 128 个字符之间"
    exit 1
fi
printf 'node_id=%s\nsecret=%s\n' "$node_id" "$node_secret" >"$node_credentials"

curl -fsS --connect-timeout 10 --max-time 120 \
    --dump-header "$node_headers" --output "$node_binary" \
    "$FLEXEDGE_SERVER_ORIGIN/api/agent/node"
header_value() {
    awk -v key="$1" 'tolower($1) == key { value=$2 } END { gsub("\\r", "", value); print value }' \
        "$node_headers"
}
expected_sha256="$(header_value 'x-flexedge-node-sha256:')"
expected_version="$(header_value 'x-flexedge-node-version:')"
case "$expected_sha256" in
    *[!0-9a-f]*|'')
        echo "Server 返回的 Node SHA-256 不正确"
        exit 1
        ;;
esac
case "$expected_version" in
    *[!0-9A-Za-z._+-]*|'')
        echo "Server 返回的 Node 版本不正确"
        exit 1
        ;;
esac
if [ "${#expected_version}" -gt 64 ]; then
    echo "Server 返回的 Node 版本长度不正确"
    exit 1
fi
if [ "${#expected_sha256}" -ne 64 ]; then
    echo "Server 返回的 Node SHA-256 长度不正确"
    exit 1
fi
actual_sha256="$(sha256sum "$node_binary")"
actual_sha256="${actual_sha256%% *}"
if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "Node 二进制 SHA-256 校验失败"
    exit 1
fi
install -d -m 0755 /opt/flexedge
if [ ! -f /opt/flexedge/credentials ] || ! cmp -s "$node_credentials" /opt/flexedge/credentials; then
    rm -rf /opt/flexedge/state
fi
install -d -m 0700 /opt/flexedge/state
install -m 0755 "$node_binary" /opt/flexedge/node
install -m 0600 "$node_credentials" /opt/flexedge/credentials
rm -f /opt/flexedge/enrollment
rm -f /opt/flexedge/node.env

cat >/etc/systemd/system/flexedge-node.service <<FLEXEDGE_SERVICE
[Unit]
Description=FlexEdge Node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=/opt/flexedge
ExecStart=/opt/flexedge/node $websocket_origin /opt/flexedge/credentials
Restart=always
RestartSec=3
UMask=0077
NoNewPrivileges=true
ProtectHome=true
ProtectSystem=strict
ReadWritePaths=/opt/flexedge

[Install]
WantedBy=multi-user.target
FLEXEDGE_SERVICE

systemctl daemon-reload
systemctl enable flexedge-node
systemctl restart flexedge-node
systemctl --no-pager --full status flexedge-node

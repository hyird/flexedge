#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
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

# The origin is also embedded in a systemd ExecStart argument. Accept only
# an authority, never shell/systemd expansion syntax or a URL path.
FLEXEDGE_SERVER_ORIGIN="${FLEXEDGE_SERVER_ORIGIN%/}"
origin_pattern='^https?://([A-Za-z0-9][A-Za-z0-9.-]*|\[[0-9A-Fa-f:.]+\])(:([0-9]{1,5}))?$'
if [[ ! "$FLEXEDGE_SERVER_ORIGIN" =~ $origin_pattern ]]; then
    echo "FLEXEDGE_SERVER_ORIGIN 必须仅包含协议、主机和可选端口"
    exit 1
fi
origin_port="${BASH_REMATCH[3]}"
if [ -n "$origin_port" ] && (( 10#$origin_port < 1 || 10#$origin_port > 65535 )); then
    echo "FLEXEDGE_SERVER_ORIGIN 端口必须在 1 到 65535 之间"
    exit 1
fi

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
case "$node_secret" in
    *[!\!-~]*)
        echo "secret 必须仅包含非空白可打印 ASCII 字符"
        exit 1
        ;;
esac

websocket_origin="${FLEXEDGE_SERVER_ORIGIN/#http/ws}"
staging_dir="$(mktemp -d /tmp/flexedge-install.XXXXXX)"
service_stopped=false
backup_ready=false
was_active=false
was_enabled=false
enable_attempted=false
start_attempted=false
transaction_paths=(/opt/flexedge/node /opt/flexedge/credentials /opt/flexedge/state
    /opt/flexedge/enrollment /opt/flexedge/node.env /etc/systemd/system/flexedge-node.service)

rollback() {
    if "$backup_ready"; then
        if "$start_attempted"; then
            systemctl stop flexedge-node.service || return 1
        fi
        if "$enable_attempted" && ! "$was_enabled"; then
            systemctl disable flexedge-node.service || return 1
        fi
        for index in "${!transaction_paths[@]}"; do
            destination="${transaction_paths[$index]}"
            rm -rf -- "$destination" || return 1
            if [ -e "$staging_dir/backup/$index" ] || [ -L "$staging_dir/backup/$index" ]; then
                cp -a -- "$staging_dir/backup/$index" "$destination" || return 1
            fi
        done
        systemctl daemon-reload || return 1
    fi
    if "$was_active"; then
        systemctl start flexedge-node.service || return 1
    fi
}

cleanup() {
    result=$?
    if "$service_stopped"; then
        if ! rollback; then
            echo "Node 安装恢复失败，保留恢复文件: $staging_dir" >&2
            exit 1
        fi
    fi
    rm -rf -- "$staging_dir"
    exit "$result"
}
trap cleanup EXIT
node_binary="$staging_dir/node"
node_headers="$staging_dir/headers"
node_credentials="$staging_dir/credentials"

printf 'node_id=%s\nsecret=%s\n' "$node_id" "$node_secret" >"$node_credentials"

curl -fsS --connect-timeout 10 --max-time 120 \
    --dump-header "$node_headers" --output "$node_binary" \
    "$FLEXEDGE_SERVER_ORIGIN/api/agent/node"
header_value() {
    awk -v key="$1" '
        tolower(substr($0, 1, index($0, ":"))) == key {
            value = substr($0, index($0, ":") + 1)
            sub(/\r$/, "", value)
            sub(/^[ \t]+/, "", value)
            sub(/[ \t]+$/, "", value)
        }
        END { print value }
    ' \
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
exec 9>/opt/flexedge/.install.lock
if ! flock -n 9; then
    echo "另一个 Node 安装正在进行"
    exit 1
fi
load_state="$(systemctl show --property=LoadState --value flexedge-node.service)"
if systemctl is-active --quiet flexedge-node.service; then was_active=true; fi
if systemctl is-enabled --quiet flexedge-node.service; then was_enabled=true; fi
if [ "$load_state" != "not-found" ]; then
    # Stop the old process before touching its binary, credentials or state.
    # A failed stop must leave all installed resources intact.
    systemctl stop flexedge-node.service
fi
service_stopped=true
mkdir "$staging_dir/backup"
for index in "${!transaction_paths[@]}"; do
    source_path="${transaction_paths[$index]}"
    if [ -e "$source_path" ] || [ -L "$source_path" ]; then
        cp -a -- "$source_path" "$staging_dir/backup/$index"
    fi
done
backup_ready=true
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
start_attempted=true
systemctl restart flexedge-node
systemctl is-active --quiet flexedge-node.service
enable_attempted=true
systemctl enable flexedge-node
service_stopped=false
systemctl --no-pager --full status flexedge-node

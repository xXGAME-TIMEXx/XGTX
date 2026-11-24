#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run this installer as root." >&2
  exit 1
fi

install_root=/opt/xgtx/indexer
data_root=/var/lib/xgtx-indexer
service_file=/etc/systemd/system/xgtx-indexer.service
nginx_snippet=/etc/nginx/snippets/xgtx-indexer.conf
nginx_limit=/etc/nginx/conf.d/xgtx-indexer-limit.conf
backup_root=/var/backups/xgtx-indexer/$(date -u +%Y%m%dT%H%M%SZ)
script_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

for required in xgtx_indexer.py xgtx-indexer.service nginx-location.conf; do
  [[ -f "${script_root}/${required}" ]] || {
    echo "Missing deployment file: ${required}" >&2
    exit 1
  }
done

command -v python3 >/dev/null
command -v systemctl >/dev/null
command -v nginx >/dev/null
command -v curl >/dev/null

mapfile -t nginx_sites < <(
  grep -ElR 'server_name[^;]*(^|[[:space:]])(www\.)?xgtxchain\.com([[:space:]]|;)' \
    /etc/nginx/sites-enabled /etc/nginx/conf.d 2>/dev/null || true
)
if [[ "${#nginx_sites[@]}" -ne 1 ]]; then
  echo "Expected exactly one enabled nginx config for xgtxchain.com; found ${#nginx_sites[@]}." >&2
  printf '  %s\n' "${nginx_sites[@]:-}" >&2
  exit 1
fi
nginx_site=${nginx_sites[0]}

install -d -m 0700 "${backup_root}"
for target in "${service_file}" "${nginx_snippet}" "${nginx_limit}" "${nginx_site}"; do
  [[ -e "${target}" ]] && cp -a -- "${target}" "${backup_root}/$(basename -- "${target}")"
done

if ! getent passwd xgtx-indexer >/dev/null; then
  useradd --system --home-dir "${data_root}" --shell /usr/sbin/nologin xgtx-indexer
fi
install -d -o root -g root -m 0755 "${install_root}"
install -d -o xgtx-indexer -g xgtx-indexer -m 0750 "${data_root}"
install -o root -g root -m 0755 "${script_root}/xgtx_indexer.py" "${install_root}/xgtx_indexer.py"
install -o root -g root -m 0644 "${script_root}/xgtx-indexer.service" "${service_file}"
install -d -o root -g root -m 0755 /etc/nginx/snippets /etc/nginx/conf.d
install -o root -g root -m 0644 "${script_root}/nginx-location.conf" "${nginx_snippet}"
if ! nginx -T 2>&1 | grep -Eq 'limit_req_zone[^;]*zone=api:'; then
  printf '%s\n' 'limit_req_zone $binary_remote_addr zone=api:10m rate=10r/s;' >"${nginx_limit}"
fi

NGINX_SITE="${nginx_site}" python3 - <<'PY'
import os
from pathlib import Path

path = Path(os.environ["NGINX_SITE"])
text = path.read_text(encoding="utf-8")
directive = "    include /etc/nginx/snippets/xgtx-indexer.conf;"
if directive in text:
    raise SystemExit(0)

depth = 0
server_start = None
server_depth = None
candidate = None
for index, char in enumerate(text):
    if char == "{":
        prefix = text[max(0, index - 40):index]
        depth += 1
        if prefix.rstrip().endswith("server"):
            server_start = index
            server_depth = depth
    elif char == "}":
        if server_start is not None and depth == server_depth:
            block = text[server_start:index]
            if "xgtxchain.com" in block:
                candidate = index
                break
            server_start = None
            server_depth = None
        depth -= 1

if candidate is None:
    raise SystemExit("Could not locate the xgtxchain.com server block")
text = text[:candidate] + directive + "\n" + text[candidate:]
path.write_text(text, encoding="utf-8")
PY

runuser -u xgtx-indexer -- /usr/bin/python3 "${install_root}/xgtx_indexer.py" \
  --once --database "${data_root}/index.sqlite3"
systemctl daemon-reload
systemctl enable xgtx-indexer
systemctl restart xgtx-indexer

curl --fail --silent --show-error http://127.0.0.1:48400/health >/dev/null
if ! nginx -t; then
  echo "nginx validation failed. Backups are in ${backup_root}; nginx was not reloaded." >&2
  exit 1
fi
systemctl reload nginx
curl --fail --silent --show-error https://xgtxchain.com/indexer/health
echo
echo "XGTX indexer deployed successfully. Backup: ${backup_root}"

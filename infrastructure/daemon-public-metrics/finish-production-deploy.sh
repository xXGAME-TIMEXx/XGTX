#!/usr/bin/env bash
set -Eeuo pipefail

EXPECTED_SHA256=3264dbf5e71141dbdaa8fa7c42ee1c5df45117458b96529d7c8869fd0988812b
CANDIDATE=/tmp/xgtxd-public-metrics.new
UNIT=xgtxd.service
MARKETS_UNIT=xgtx-native-markets.service
MARKETS=/opt/xgtx-native-dex-web/markets.py
INDEXER=/opt/xgtx/indexer/xgtx_indexer.py
INDEXER_UNIT=xgtx-indexer.service
DROPIN=/etc/systemd/system/xgtxd.service.d/50-public-metrics-libs.conf
TEMP_KEY_MARKER=xgtx-github-actions

if [[ ${EUID} -ne 0 ]]; then
  echo "ERROR: run as root" >&2
  exit 1
fi

revoke_temp_key() {
  if [[ -f /root/.ssh/authorized_keys ]]; then
    sed -i "/${TEMP_KEY_MARKER}/d" /root/.ssh/authorized_keys
  fi
}

unit_value="$(systemctl show -p ExecStart --value "${UNIT}")"
BINARY="$(printf '%s\n' "${unit_value}" | sed -n 's/.*path=\([^ ;}]*\).*/\1/p')"
[[ -n "${BINARY}" && -x "${BINARY}" && "${BINARY}" = /* ]]
systemctl is-active --quiet "${UNIT}"
systemctl is-active --quiet "${MARKETS_UNIT}"
systemctl is-active --quiet "${INDEXER_UNIT}"
[[ -f "${MARKETS}" ]]
[[ -f "${INDEXER}" ]]
[[ -x "${CANDIDATE}" ]]
[[ "$(sha256sum "${CANDIDATE}" | awk '{print $1}')" = "${EXPECTED_SHA256}" ]]

current_version="$("${BINARY}" --version 2>&1)"
candidate_version="$("${CANDIDATE}" --version 2>&1)"
current_base="$(grep -oE 'v[0-9]+(\.[0-9]+){3}' <<<"${current_version}" | head -n1)"
candidate_base="$(grep -oE 'v[0-9]+(\.[0-9]+){3}' <<<"${candidate_version}" | head -n1)"
[[ -n "${current_base}" && "${current_base}" = "${candidate_base}" ]]

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
BACKUP="/var/backups/xgtxd-public-metrics/${stamp}"
mkdir -p "${BACKUP}"
cp -a "${BINARY}" "${BACKUP}/xgtxd"
cp -a "${MARKETS}" "${BACKUP}/markets.py"
cp -a "${INDEXER}" "${BACKUP}/xgtx_indexer.py"
systemctl cat "${UNIT}" > "${BACKUP}/xgtxd.service.txt"
sha256sum "${BACKUP}/xgtxd" > "${BACKUP}/xgtxd.SHA256SUMS"
dropin_existed=0
if [[ -f "${DROPIN}" ]]; then
  cp -a "${DROPIN}" "${BACKUP}/50-public-metrics-libs.conf"
  dropin_existed=1
fi

rollback() {
  rc="${1:-1}"
  trap - ERR INT TERM
  set +e
  echo "Deployment failed; restoring ${BACKUP}" >&2
  install -m 0755 "${BACKUP}/xgtxd" "${BINARY}"
  install -m 0644 "${BACKUP}/markets.py" "${MARKETS}"
  install -m 0644 "${BACKUP}/xgtx_indexer.py" "${INDEXER}"
  if [[ "${dropin_existed}" -eq 1 ]]; then
    install -m 0644 "${BACKUP}/50-public-metrics-libs.conf" "${DROPIN}"
  else
    rm -f "${DROPIN}"
  fi
  systemctl daemon-reload
  systemctl restart "${UNIT}"
  for _ in $(seq 1 30); do
    systemctl is-active --quiet "${UNIT}" && ss -ltn | grep -q '127.0.0.1:48081' && break
    sleep 2
  done
  systemctl is-active --quiet "${UNIT}"
  systemctl restart "${MARKETS_UNIT}"
  systemctl is-active --quiet "${MARKETS_UNIT}"
  systemctl restart "${INDEXER_UNIT}"
  systemctl is-active --quiet "${INDEXER_UNIT}"
  revoke_temp_key
  rm -f "${INDEXER_NEW:-}" /run/xgtx-public-metrics-rollback
  exit "${rc}"
}
trap 'rollback $?' ERR
trap 'rollback 130' INT TERM

printf '%s\n%s\n' "${BACKUP}" "${BINARY}" > /run/xgtx-public-metrics-rollback

# Preserve the gateway's strict allowlist while adding the two audited,
# non-wallet public metrics emitted by the candidate daemon.
python3 - "${MARKETS}" <<'PY'
from pathlib import Path
import sys
path=Path(sys.argv[1])
text=path.read_text()
old="            assets.append(dict(id=ident(row['id']),issuer=ident(row['issuer']),ticker=row['ticker'],decimals=row['decimals'],supply=units(row['supply'])))"
new="            assets.append(dict(id=ident(row['id']),issuer=ident(row['issuer']),ticker=row['ticker'],decimals=row['decimals'],supply=units(row['supply']),holders=units(row['holders']),treasury_balance=units(row['treasury_balance'])))"
if text.count(old) != 1:
    raise SystemExit("Refusing to patch unexpected markets.py")
path.write_text(text.replace(old,new))
PY
python3 -m py_compile "${MARKETS}"

# Install the immutable, tested indexer revision. Production previously ran an
# older serializer that could not expose treasury-balance coverage.
INDEXER_REF=d0aa3865809b37503b98c1e1e5900419c50f4429
INDEXER_NEW=/tmp/xgtx_indexer.public-metrics.py
curl --fail --silent --show-error --location --proto '=https' --tlsv1.2 \
  "https://raw.githubusercontent.com/xXGAME-TIMEXx/XGTX/${INDEXER_REF}/infrastructure/indexer/xgtx_indexer.py" \
  -o "${INDEXER_NEW}"
grep -q 'has_treasury.*treasury_balance_atomic' "${INDEXER_NEW}"
grep -q 'duplicate snapshot asset set differs' "${INDEXER_NEW}"
python3 -m py_compile "${INDEXER_NEW}"
install -m 0644 "${INDEXER_NEW}" "${INDEXER}"

# The production service normally prefers bundled runtime libraries. The
# candidate is built and tested against Ubuntu 24.04 system libraries. Retain
# the bundled path when compatible; otherwise use a reversible service drop-in.
if env LD_LIBRARY_PATH=/opt/xgtx/lib "${CANDIDATE}" --version >/dev/null 2>&1; then
  rm -f "${DROPIN}"
else
  install -d -m 0755 "$(dirname "${DROPIN}")"
  printf '%s\n' '[Service]' 'Environment="LD_LIBRARY_PATH="' > "${DROPIN}"
fi

systemctl daemon-reload
install -m 0755 "${CANDIDATE}" "${BINARY}"
systemctl restart "${UNIT}"

for _ in $(seq 1 45); do
  if systemctl is-active --quiet "${UNIT}" &&
     ss -ltn | grep -q '127.0.0.1:48081' &&
     [[ "$(sha256sum "${BINARY}" | awk '{print $1}')" = "${EXPECTED_SHA256}" ]]; then
    break
  fi
  sleep 2
done
systemctl is-active --quiet "${UNIT}"
ss -ltn | grep -q '127.0.0.1:48081'
[[ "$(sha256sum "${BINARY}" | awk '{print $1}')" = "${EXPECTED_SHA256}" ]]
sleep 8
systemctl is-active --quiet "${UNIT}"
systemctl restart "${MARKETS_UNIT}"
systemctl is-active --quiet "${MARKETS_UNIT}"

public_ok=0
for _ in $(seq 1 60); do
  if state="$(curl --fail --silent --show-error https://xgtxchain.com/dex/api/state 2>/dev/null)" &&
     health="$(curl --fail --silent --show-error https://xgtxchain.com/indexer/health 2>/dev/null)" &&
     python3 - "${state}" "${health}" <<'PY'
import json,sys
state=json.loads(sys.argv[1])
health=json.loads(sys.argv[2])
assert health.get("ok") is True
assert state.get("assets")
assert all("holders" in asset and "treasury_balance" in asset for asset in state["assets"])
PY
  then
    public_ok=1
    break
  fi
  sleep 5
done
[[ "${public_ok}" -eq 1 ]]

systemctl restart "${INDEXER_UNIT}"
systemctl is-active --quiet "${INDEXER_UNIT}"

coverage_ok=0
for _ in $(seq 1 36); do
  if summary="$(curl --fail --silent --show-error https://xgtxchain.com/indexer/v1/summary 2>/dev/null)" &&
     python3 - "${summary}" <<'PY'
import json,sys
summary=json.loads(sys.argv[1])
coverage=summary.get("coverage") or {}
assert coverage.get("holder_count") is True
assert coverage.get("treasury_balance") is True
PY
  then
    coverage_ok=1
    break
  fi
  sleep 5
done
[[ "${coverage_ok}" -eq 1 ]]

trap - ERR INT TERM
rm -f "${CANDIDATE}" "${INDEXER_NEW}" /run/xgtx-public-metrics-rollback
revoke_temp_key
echo "PASS: XGTX daemon public metrics deployed"
echo "Backup: ${BACKUP}"
echo "Installed SHA256: ${EXPECTED_SHA256}"
echo "Indexer SHA256: $(sha256sum "${INDEXER}" | awk '{print $1}')"
echo "Gateway SHA256: $(sha256sum "${MARKETS}" | awk '{print $1}')"

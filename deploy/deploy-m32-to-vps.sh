#!/bin/bash
# deploy-m32-to-vps.sh — deploy the M32 signal-log engine to the VPS (no sudo path)
#
# sudo needs a password (api is not passwordless) → use a self-healing upgrade via
# systemd Restart=always:
#   1. scp the new bundle (binary + lib/) + the derived trading.toml + the trial tools
#   2. Back up the old binary as api (local rollback point)
#   3. kill -9 the engine → root systemd starts the new binary within 5s
#   (signals-only mode holds no positions, so a restart carries no capital risk;
#   the engine runs as api, and api can kill it)
#   4. Health check: systemctl is-active / strategies / signals table + persistence
#
# Usage: bash deploy/deploy-m32-to-vps.sh
# Prerequisites: deploy/build-cloud-bundle.sh has finished + make-vps-config.py has run

set -euo pipefail
cd "$(dirname "$0")/.."

VPS=api
BUNDLE=deploy/cloud-out/pulseTrader
VPS_DIR=/home/api/pulseTrader
TOML=deploy/cloud-out/trading.vps-futures.toml
PATCH_VER=$(git rev-parse --short HEAD)

echo "==> [1/4] scp the new bundle (binary + lib) to the VPS"
# A running executable cannot be overwritten in place (ETXTBSY) → scp to a
# temporary name and mv over it (rename is harmless to the running process:
# the old process keeps the old inode, and the new file is only exec'd after
# the kill and restart)
scp -r "$BUNDLE"/lib "${VPS}:${VPS_DIR}/"
scp "$BUNDLE"/pulsetrader "${VPS}:${VPS_DIR}/pulsetrader.new"

echo "==> [2/4] scp the derived trading.toml + the trial toolchain"
scp "$TOML" "${VPS}:${VPS_DIR}/trading.toml"
ssh "${VPS}" "mkdir -p ${VPS_DIR}/tools/trial"
scp tools/trial/paper_engine.py tools/trial/run_replay.py \
    tools/trial/daily_rollup.py tools/trial/calibrate_fill_model.py \
    "${VPS}:${VPS_DIR}/tools/trial/"

echo "==> [3/4] back up the old binary → mv over it → kill -9 to trigger the systemd self-healing restart"
ssh "${VPS}" "
  set -e
  cp ${VPS_DIR}/pulsetrader ${VPS_DIR}/pulsetrader.bak-${PATCH_VER//\//-}
  mv -f ${VPS_DIR}/pulsetrader.new ${VPS_DIR}/pulsetrader
  pkill -9 -f '^./pulsetrader ' || pkill -9 -f 'pulsetrader trade' || true
  sleep 1
"

echo "==> [4/4] health check (wait for self-heal + engine readiness)"
for i in $(seq 1 12); do
  sleep 5
  ACTIVE=$(ssh "${VPS}" "systemctl is-active pulsetrader 2>/dev/null || echo unknown")
  if [ "$ACTIVE" = "active" ]; then break; fi
done
ssh "${VPS}" "
  systemctl is-active pulsetrader
  tail -3 ${VPS_DIR}/engine.out
  python3 - <<'PY'
import sqlite3
con = sqlite3.connect('${VPS_DIR}/data/trades.db')
t = con.execute(\"SELECT name FROM sqlite_master WHERE name='signals'\").fetchone()
print('signals table:', 'present' if t else 'MISSING!')
if t:
    n = con.execute('SELECT COUNT(*) FROM signals').fetchone()[0]
    print('signals rows:', n)
con.close()
PY
"
echo "==> Done. If the health check fails: ssh ${VPS} 'cp ${VPS_DIR}/pulsetrader.bak-${PATCH_VER//\//-} ${VPS_DIR}/pulsetrader' and then kill -9 again to roll back"

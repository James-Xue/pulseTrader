#!/bin/bash
# deploy-m32-to-vps.sh — 部署 M32 信号日志引擎到 VPS(无 sudo 路径)
#
# sudo 需要密码(api 非免密)→ 走 systemd Restart=always 自愈升级:
#   1. scp 新 bundle(二进制 + lib/)+ 派生 trading.toml + trial 工具
#   2. api 身份备份旧二进制(本地回滚点)
#   3. kill -9 引擎 → root systemd 5s 内拉起新二进制
#   (信号仅模式零持仓,重启无资金风险;引擎以 api 运行,api 可杀)
#   4. 健康验证:systemctl is-active / strategies / signals 表 + 落库
#
# 用法: bash deploy/deploy-m32-to-vps.sh
# 前置: deploy/build-cloud-bundle.sh 完成 + make-vps-config.py 已跑

set -euo pipefail
cd "$(dirname "$0")/.."

VPS=api
BUNDLE=deploy/cloud-out/pulseTrader
VPS_DIR=/home/api/pulseTrader
TOML=deploy/cloud-out/trading.vps-futures.toml
PATCH_VER=$(git rev-parse --short HEAD)

echo "==> [1/4] scp 新 bundle(二进制+lib)到 VPS"
scp -r "$BUNDLE"/pulsetrader "$BUNDLE"/lib "${VPS}:${VPS_DIR}/"

echo "==> [2/4] scp 派生 trading.toml + trial 工具链"
scp "$TOML" "${VPS}:${VPS_DIR}/trading.toml"
ssh "${VPS}" "mkdir -p ${VPS_DIR}/tools/trial"
scp tools/trial/paper_engine.py tools/trial/run_replay.py \
    tools/trial/daily_rollup.py tools/trial/calibrate_fill_model.py \
    "${VPS}:${VPS_DIR}/tools/trial/"

echo "==> [3/4] 备份旧二进制 → kill -9 触发 systemd 自愈重启"
ssh "${VPS}" "
  set -e
  cp ${VPS_DIR}/pulsetrader ${VPS_DIR}/pulsetrader.bak-${PATCH_VER//\//-}
  pkill -9 -f '^./pulsetrader ' || pkill -9 -f 'pulsetrader trade' || true
  sleep 1
"

echo "==> [4/4] 健康验证(等自愈 + 引擎就绪)"
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
print('signals 表:', '存在' if t else '缺失!')
if t:
    n = con.execute('SELECT COUNT(*) FROM signals').fetchone()[0]
    print('signals 行数:', n)
con.close()
PY
"
echo "==> 完成。如健康检查异常:ssh ${VPS} 'cp ${VPS_DIR}/pulsetrader.bak-${PATCH_VER//\//-} ${VPS_DIR}/pulsetrader' 后再次 kill -9 回滚"

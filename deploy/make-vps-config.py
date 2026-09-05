#!/usr/bin/env python3
"""make-vps-config.py — 从仓库权威 trading.toml 派生 VPS futures-only 配置。

背景 (2026-09-04):主网引擎迁至 VPS 后跑期货专用配置,原因是主网 API key
暂无 tradfi/CFD 权限(403 FORBIDDEN)。手工派生曾两次踩坑:① 忘记把本机
Clash 代理 proxyUrl=127.0.0.1:7897 清空,导致引擎连不上行情;② 派生产物
与仓库配置漂移。本脚本把派生固化为可重复步骤,将来改仓库 trading.toml 后
重跑一遍即可得到 VPS 版。

用法:
    python3 deploy/make-vps-config.py [输出路径]
    默认输出: deploy/cloud-out/trading.vps-futures.toml

转换规则:
    1. proxyUrl 清空(本机 7897 代理勿带入 VPS)
    2. symbols 去掉 XAUUSD / XAGUSD(CFD)
    3. active_market → futures
    4. 删除整个 TradFi CFD 策略区块(注释头到 [risk] 之前)
    5. tomllib 校验 + 打印实例清单供人工核对
"""
import re
import sys
import tomllib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "trading.toml"
OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "deploy/cloud-out/trading.vps-futures.toml"

s = SRC.read_text(encoding="utf-8")

# 1) proxyUrl 清空(本机 Clash 地址)
s = re.sub(r'(proxyUrl\s*=\s*)"[^"]*"', r'\1""', s, count=1)

# 2) symbols 去掉 CFD 品种
s = re.sub(
    r"symbols = \[[^\]]*\]",
    'symbols = ["BTC_USDT", "SNDK_USDT", "UNITREE_USDT", "ETH_USDT"]',
    s,
    count=1,
)

# 3) active_market → futures(注释同步)
s = re.sub(
    r'active_market = "[^"]*"[^\n]*',
    'active_market = "futures"   # VPS 派生:主网 key 暂无 tradfi/CFD 权限(补权后改回 cfd 并恢复完整版)',
    s,
    count=1,
)

# 4) 删除 TradFi CFD 策略区块
start = s.index("# ── TradFi CFD strategies")
end = s.index("# ── Risk Management")
s = s[:start] + s[end:]

# 5) 校验 + 汇报
d = tomllib.loads(s)
n_cfd = sum(1 for i in d["strategy"]["instances"] if i.get("market_type") == "cfd")
assert n_cfd == 0, f"CFD 实例残留 {n_cfd}"
assert not any(x in d["symbols"] for x in ("XAUUSD", "XAGUSD")), "symbols 残留 CFD"
insts = sorted(f"{i['name']}_{i['symbol']}" for i in d["strategy"]["instances"])
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(s, encoding="utf-8")
print(f"已生成: {OUT}")
print(f"symbols:   {d['symbols']}")
print(f"active_market: {d['active_market']}")
print(f"proxyUrl:  {d['exchange'].get('proxyUrl', '')!r}")
print(f"实例数:    {len(insts)} (cfd={n_cfd})")
for i in insts:
    print(f"  - {i}")

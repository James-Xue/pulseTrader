#!/usr/bin/env python3
"""Measure per-turn latency and throughput from Claude Code session transcripts.

One "call" = one API round-trip, i.e. a maximal run of consecutive assistant
entries sharing the same usage counters (Claude Code writes one chunk per
content block, all carrying identical usage).

Per call we record:
  ttft  — previous non-assistant event → first chunk   (queueing + prompt eval)
  gen   — first chunk → last chunk                     (actual generation)
  dur   — previous non-assistant event → last chunk    (total, user-perceived)
  tok/s — output_tokens / gen

Splitting ttft from gen is what tells "the prompt got huge" apart from "the
provider got slow": a bloated prompt raises ttft, a slow provider lowers tok/s.
"""

import json
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

PROJ = Path.home() / ".claude/projects/-home-joey-1-Code-09-pulseTrader"
LOCAL = lambda ts: datetime.fromtimestamp(ts).strftime  # noqa: E731


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp() if s else None


def stamp(ts, fmt="%m-%d %H:%M"):
    return datetime.fromtimestamp(ts, timezone.utc).astimezone().strftime(fmt)


def load_calls(path):
    """Return (calls, tool_spans) for one transcript."""
    events = []
    for line in path.open(errors="replace"):
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except json.JSONDecodeError:
            continue
        ts = parse_ts(d.get("timestamp"))
        if ts is not None:
            events.append((ts, d))
    events.sort(key=lambda e: e[0])

    calls, tool_spans, pending = [], [], {}
    prev_non_asst = None
    cur = None

    def close():
        if cur and cur["first"] is not None:
            cur["ttft"] = cur["first"] - cur["anchor"]
            cur["gen"] = cur["last"] - cur["first"]
            cur["dur"] = cur["last"] - cur["anchor"]
            calls.append(cur)

    for ts, d in events:
        t = d.get("type")
        msg = d.get("message") or {}
        content = msg.get("content")
        blocks = content if isinstance(content, list) else []

        if t == "assistant":
            u = msg.get("usage") or {}
            sig = (u.get("input_tokens"), u.get("cache_read_input_tokens"),
                   u.get("cache_creation_input_tokens"), u.get("output_tokens"))
            if cur and cur["sig"] == sig:
                cur["last"] = ts
            else:
                close()
                cur = {
                    "sig": sig, "anchor": prev_non_asst, "first": ts, "last": ts,
                    "prompt": (u.get("input_tokens") or 0)
                    + (u.get("cache_read_input_tokens") or 0)
                    + (u.get("cache_creation_input_tokens") or 0),
                    "out": u.get("output_tokens") or 0,
                    "session": path.stem,
                    "blocks": set(),
                }
            cur["blocks"] |= {b.get("type") for b in blocks if isinstance(b, dict)}
            # register tool_use on EVERY chunk — continuation chunks carry them too
            for b in blocks:
                if isinstance(b, dict) and b.get("type") == "tool_use":
                    pending[b.get("id")] = (ts, b.get("name"))
        else:
            for b in blocks:
                if isinstance(b, dict) and b.get("type") == "tool_result":
                    tid = b.get("tool_use_id")
                    if tid in pending:
                        t0, name = pending.pop(tid)
                        tool_spans.append((t0, ts, name))
            if ts is not None:
                prev_non_asst = ts
    close()
    return [c for c in calls if c["anchor"] is not None], tool_spans


def main():
    files = [Path(f) for f in sys.argv[1:]] or sorted(PROJ.glob("*.jsonl"))
    day = defaultdict(lambda: {"calls": [], "tools": []})
    sess = []

    for f in files:
        calls, tools = load_calls(f)
        if not calls:
            continue
        for c in calls:
            day[stamp(c["last"], "%Y-%m-%d")]["calls"].append(c)
        for t0, t1, name in tools:
            day[stamp(t1, "%Y-%m-%d")]["tools"].append((t1 - t0, name))
        sess.append((calls[0]["last"], f.stem[:8], len(calls),
                     sum(c["dur"] for c in calls), sum(t1 - t0 for t0, t1, _ in tools)))

    med = lambda v: sorted(v)[len(v) // 2] if v else float("nan")  # noqa: E731

    print("=" * 108)
    print("按天 —— 时间都去哪了   dur=整轮模型往返(首token→末chunk的总跨度, 即你等待时间)")
    print("=" * 108)
    print(f"{'day':12s} {'calls':>6s} {'med_prompt':>11s} {'med_out':>8s} "
          f"{'p50 dur':>8s} {'p50 tool':>9s} {'tools':>6s} {'model_min':>10s} {'tool_min':>9s} {'tool%':>6s}")
    for d in sorted(day):
        cs, tls = day[d]["calls"], [t for t, _ in day[d]["tools"]]
        m_min = sum(c["dur"] for c in cs) / 60
        t_min = sum(tls) / 60
        tot = m_min + t_min
        print(f"{d:12s} {len(cs):6d} {med([c['prompt'] for c in cs]):11,.0f} "
              f"{med([c['out'] for c in cs]):8,.0f} "
              f"{med([c['dur'] for c in cs]):7.1f}s {med(tls):8.2f}s "
              f"{len(tls):6d} {m_min:10.1f} {t_min:9.1f} "
              f"{100*t_min/tot if tot else 0:5.1f}%")

    print()
    print("=" * 108)
    print("每会话分解(active = 模型在跑 + 工具在跑,不含你离开的间隙)")
    print("=" * 108)
    print(f"{'start':13s} {'sess':9s} {'calls':>6s} {'model_min':>10s} "
          f"{'tool_min':>9s} {'active_min':>11s} {'tool%':>6s}")
    for ts, sid, n, model, tool in sorted(sess)[-20:]:
        act = model + tool
        print(f"{stamp(ts, '%m-%d %H:%M'):13s} {sid:9s} {n:6d} {model/60:10.1f} "
              f"{tool/60:8.1f} {act/60:10.1f} {100*tool/act if act else 0:5.1f}%")


if __name__ == "__main__":
    main()

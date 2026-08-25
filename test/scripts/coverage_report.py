#!/usr/bin/env python3
"""覆盖率报告生成器。

输入：
  mode="llvm"  input=<llvm-cov export -summary-only 的 JSON>
  mode="lcov"  input=<lcov .info 文件>
  [baseline]  同 mode 的第二个输入，用于产出 before/after 对比表。

输出：
  docs/reports/coverage-<date>.md     汇总报告（markdown）
  并在 stdout 打印模块覆盖率表。

只统计项目 include/ + src/ 下的源码（输入侧已由 coverage.sh 过滤 test/generated/系统头）。
行覆盖率 = covered lines / total lines；函数、区域同理。
"""
import json
import os
import sys
import datetime
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


# ------------------------------------------------------------------ helpers

def module_of(rel):
    """把相对源码路径归类到模块（按目录归组，不含文件名）。"""
    parts = rel.replace("\\", "/").split("/")
    if parts and parts[0] in ("src", "include"):
        parts = parts[1:]
    if not parts:
        return "(root)"
    if parts[0] == "framework":
        return "framework"
    if parts[0] == "mail_system":
        # 去掉文件名，按目录归组：mail_system/back/<subdir>
        dirparts = parts[:-1]
        return "/".join(dirparts[:4]) if len(dirparts) >= 4 else "/".join(dirparts)
    return parts[0]


def norm_path(p):
    p = os.path.normpath(p)
    try:
        rel = os.path.relpath(p, ROOT)
        if not rel.startswith(".."):
            return rel
    except ValueError:
        pass
    # 无法相对化（跨盘符/不同根），按已知前缀回退
    for prefix in ("include/", "src/"):
        if prefix in p:
            return p[p.index(prefix):]
    return p


# ------------------------------------------------------------------ parsers

def parse_llvm(path):
    with open(path) as f:
        data = json.load(f)
    d = data["data"][0]
    files = []
    for f_ in d["files"]:
        s = f_.get("summary", {})
        files.append({
            "file": norm_path(f_["filename"]),
            "lines": s.get("lines", {}).get("percent", 0.0),
            "lcov_lines": (s.get("lines", {}).get("covered", 0),
                           s.get("lines", {}).get("count", 0)),
            "func": s.get("functions", {}).get("percent", 0.0),
            "reg": s.get("regions", {}).get("percent", 0.0),
        })
    t = d.get("totals", {})
    totals = {
        "lines": t.get("lines", {}).get("percent", 0.0),
        "func": t.get("functions", {}).get("percent", 0.0),
        "reg": t.get("regions", {}).get("percent", 0.0),
    }
    return files, totals


def parse_lcov(path):
    files = []
    cur = None
    totals_lines = [0, 0]
    with open(path) as f:
        for line in f:
            if line.startswith("SF:"):
                cur = {"file": norm_path(line[3:].strip()),
                       "da": [], "fn": 0, "fnda": 0}
            elif cur is not None:
                if line.startswith("DA:"):
                    _, rest = line[3:].strip().split(",", 1)
                    cur["da"].append(int(rest))
                    totals_lines[1] += 1
                elif line.startswith("FN:"):
                    cur["fn"] += 1
                elif line.startswith("FNDA:"):
                    cnt = int(line[5:].strip().split(",")[0])
                    if cnt > 0:
                        cur["fnda"] += 1
                    else:
                        cur["fn"] += 1
                elif line.startswith("end_of_record"):
                    if cur is not None:
                        files.append(cur)
                    cur = None
    out = []
    for c in files:
        n = len(c["da"])
        covered = sum(1 for x in c["da"] if x > 0)
        lc_lines = (covered, n)
        out.append({
            "file": c["file"],
            "lines": (100.0 * covered / n) if n else 0.0,
            "lcov_lines": lc_lines,
            "func": (100.0 * c["fnda"] / c["fn"]) if c["fn"] else 0.0,
            "reg": None,
        })
    t_covered = sum(1 for x in out if x["lines"] > 0)
    totals = {"lines": 100.0 * sum(x["lcov_lines"][0] for x in out) /
                      max(1, sum(x["lcov_lines"][1] for x in out)),
              "func": None, "reg": None}
    return out, totals


# ------------------------------------------------------------------ aggregation

def aggregate(files):
    m_lines = defaultdict(lambda: [0, 0])   # module -> [covered, total]
    m_func = defaultdict(lambda: [0, 0])    # module -> [covered, total]
    f_lines = {}
    for f_ in files:
        mod = module_of(f_["file"])
        cov, tot = f_["lcov_lines"]
        m_lines[mod][0] += cov
        m_lines[mod][1] += tot
        # 函数聚合：llvm 只给 percent，用 平均分估总覆盖 不严谨；仅对 lcov 有逐函数计数。
        f_lines[f_["file"]] = f_
    return m_lines, f_lines


def pct(cov, tot):
    return (100.0 * cov / tot) if tot else 0.0


def write_report(mode, files, totals, baseline_files=None):
    date = datetime.date.today().isoformat()
    m_lines, f_lines = aggregate(files)
    b_lines = None
    if baseline_files:
        b_lines, _ = aggregate(baseline_files)

    total_cov = sum(v[0] for v in m_lines.values())
    total_tot = sum(v[1] for v in m_lines.values())

    mods = sorted(m_lines)
    rows = []
    for m in mods:
        cov, tot = m_lines[m]
        cell = f"{pct(cov, tot):6.1f}%  ({cov}/{tot})"
        if b_lines and m in b_lines:
            bcov, btot = b_lines[m]
            b = pct(bcov, btot)
            cur = pct(cov, tot)
            delta = cur - b
            cell += f"  (Δ {delta:+.1f}pp, 基线 {b:.1f}%)"
        rows.append((m, cell))

    # 框架层逐文件
    fw_files = sorted((f_ for f_ in files if module_of(f_["file"]) == "framework"),
                      key=lambda x: x["lines"])
    fw_rows = [(f_["file"], f_["lcov_lines"][0], f_["lcov_lines"][1],
                f_["lines"], f_["func"]) for f_ in fw_files]

    # 覆盖率最低 Top15（全部项目文件）
    bottom = sorted(files, key=lambda x: x["lines"])[:15]

    comp = ""
    if b_lines:
        bcov = sum(v[0] for v in b_lines.values())
        btot = sum(v[1] for v in b_lines.values())
        comp = f"\n总行覆盖率：**{pct(total_cov, total_tot):.1f}%**"
        comp += f"（基线 {pct(bcov, btot):.1f}%，Δ **{pct(total_cov, total_tot) - pct(bcov, btot):+.1f}pp**）\n"

    lines = []
    lines.append(f"# 覆盖率报告 — {date}\n")
    lines.append("由 `test/scripts/coverage.sh` 生成。行覆盖率 = 已覆盖行 / 可执行行。")
    lines.append("与 [test/README.md](../../test/README.md) 的 FSM 状态覆盖矩阵互补：矩阵是行为覆盖，这里是源码行覆盖。\n")
    lines.append(f"```\n复现：bash test/scripts/coverage.sh\n工具链：{mode}\n```\n")
    lines.append(f"## 总览{comp}")
    lines.append("\n| 指标 | 覆盖 | 百分比 |")
    lines.append("|------|------|--------|")
    if mode == "llvm":
        lines.append(f"| 行 | {total_cov}/{total_tot} | **{pct(total_cov, total_tot):.1f}%** |")
        lines.append(f"| 函数 | — | {totals['func']:.1f}% |")
        lines.append(f"| 区域 | — | {totals['reg']:.1f}% |")
    else:
        lines.append(f"| 行 | {total_cov}/{total_tot} | **{pct(total_cov, total_tot):.1f}%** |")

    lines.append("\n## 模块覆盖率\n")
    lines.append("| 模块 | 行覆盖 |")
    lines.append("|------|--------|")
    for m, cell in rows:
        lines.append(f"| {m} | {cell} |")

    lines.append("\n## framework/ 逐文件\n")
    lines.append("| 文件 | 行覆盖 | 百分比 | 函数% |")
    lines.append("|------|--------|--------|--------|")
    for rel, cov, tot, lp, fp in fw_rows:
        lines.append(f"| `{rel}` | {cov}/{tot} | {lp:.1f}% | "
                     f"{fp:.1f}%" if fp is not None else f"| `{rel}` | {cov}/{tot} | {lp:.1f}% | — |")

    lines.append("\n## 覆盖率最低 Top 15\n")
    lines.append("| 文件 | 行 | 百分比 |")
    lines.append("|------|-----|--------|")
    for f_ in bottom:
        lines.append(f"| `{f_['file']}` | {f_['lcov_lines'][0]}/{f_['lcov_lines'][1]} | {f_['lines']:.1f}% |")

    out = "\n".join(lines) + "\n"

    report_path = os.path.join(ROOT, "docs", "reports", f"coverage-{date}.md")
    os.makedirs(os.path.dirname(report_path), exist_ok=True)
    with open(report_path, "w") as f:
        f.write(out)
    print(out)
    print(f"\n==> 已写入 {report_path}")


# ------------------------------------------------------------------ main

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    mode, inp = sys.argv[1], sys.argv[2]
    baseline = sys.argv[3] if len(sys.argv) > 3 else None
    if mode == "llvm":
        files, totals = parse_llvm(inp)
        b_files = parse_llvm(baseline)[0] if baseline else None
    elif mode == "lcov":
        files, totals = parse_lcov(inp)
        b_files = parse_lcov(baseline)[0] if baseline else None
    else:
        sys.exit(f"unknown mode: {mode}")
    write_report(mode, files, totals, b_files)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
日志还原工具。

读取 LOG_PURE 产生的纯日志文件 (hash|arg1|...|ts_ms\n) 和映射表 JSON，
将每行还原为带时间戳的可读日志。

增量映射表兼容性：
  映射表只追加不删除 → 任意新旧映射表都能还原包含对应 hash 的日志
  最新映射表是全部旧版映射表的超集 → 客户丢表后用公司最新表也能还原

用法:
  python tools/log_restore.py <pure.log> [--map pure_map.json] [--out readable.log]

示例:
  python tools/log_restore.py logs/pure.log --map logs/pure_map.json
  python tools/log_restore.py logs/pure.log --out -          # 输出到 stdout
  python tools/log_restore.py logs/pure.log -f               # tail -f 实时还原
"""

import sys
import os
import json
import time
import argparse
import re
from datetime import datetime
from typing import Optional

_LINE_RE = re.compile(r'^(0x[0-9a-f]+)((?:\|[^|]*)*)$')


def load_map(map_path: str) -> dict:
    with open(map_path) as f:
        return json.load(f)


def format_ts(ts_ms: int) -> str:
    """毫秒时间戳 → 可读时间字符串"""
    try:
        dt = datetime.fromtimestamp(ts_ms / 1000.0)
        return dt.strftime('%Y-%m-%d %H:%M:%S.') + f'{ts_ms % 1000:03d}'
    except (ValueError, OSError):
        return str(ts_ms)


def restore_line(line: str, entries: dict) -> str:
    """单行还原"""
    line = line.strip()
    if not line:
        return line

    m = _LINE_RE.match(line)
    if not m:
        return line

    hash_hex = m.group(1)
    rest = m.group(2)
    fields = rest.split('|')[1:] if rest else []

    # 最后一个字段是时间戳 (毫秒数)
    ts_ms = 0
    ts_str = ''
    if fields:
        try:
            ts_ms = int(fields[-1])
            ts_str = format_ts(ts_ms)
            args = fields[:-1]  # 去掉时间戳, 剩下的才是模板参数
        except ValueError:
            args = fields
    else:
        args = []

    entry = entries.get(hash_hex)
    if not entry:
        return f"[{ts_str}] [UNKNOWN:{hash_hex}] {'|'.join(args)}"

    fmt_str = entry['fmt']
    module = entry.get('module', '?')
    level = entry.get('level', '?')

    if '{}' in fmt_str:
        result = fmt_str
        i = 0
        while '{}' in result and i < len(args):
            result = result.replace('{}', args[i], 1)
            i += 1
        if '{}' in result:
            result += ' [MISSING_ARGS: need ' + str(result.count('{}')) + ']'
        if i < len(args):
            result += ' [EXTRA_ARGS: ' + '|'.join(args[i:]) + ']'
    else:
        result = fmt_str

    return f"[{ts_str}] [{level}][{module}] {result}"


def main():
    parser = argparse.ArgumentParser(
        description='Restore pure log (hash|args|ts) to readable log using mapping table')
    parser.add_argument('logfile', nargs='?', default=None,
                        help='Path to pure.log (default: logs/pure.log)')
    parser.add_argument('--map', default=None, help='Mapping table JSON')
    parser.add_argument('--out', default=None,
                        help='Output file (default: <logfile>.readable, or stdout if -f)')
    parser.add_argument('--follow', '-f', action='store_true',
                        help='Like tail -f: continuously read new lines')
    args = parser.parse_args()

    proj_dir = os.getcwd()
    logfile = args.logfile or os.path.join(proj_dir, 'logs', 'pure.log')
    map_path = args.map or os.path.join(proj_dir, 'logs', 'pure_map.json')

    if not os.path.exists(logfile):
        print(f"ERROR: Log file not found: {logfile}", file=sys.stderr)
        sys.exit(1)
    if not os.path.exists(map_path):
        print(f"ERROR: Map file not found: {map_path}", file=sys.stderr)
        sys.exit(1)

    entries = load_map(map_path)['entries']
    print(f"Loaded {len(entries)} template mappings", file=sys.stderr)

    out_path = args.out
    if not out_path and not args.follow:
        out_path = f"{logfile}.readable"
    out_f = open(out_path, 'w') if out_path and out_path != '-' else sys.stdout

    try:
        if args.follow:
            _follow_mode(logfile, entries, out_f)
        else:
            count, unknown = _batch_mode(logfile, entries, out_f)
            print(f"Restored {count} lines ({unknown} unknown hashes)", file=sys.stderr)
    except KeyboardInterrupt:
        pass
    finally:
        if out_f is not sys.stdout:
            out_f.close()
            if out_path:
                print(f"Restored log saved to: {out_path}", file=sys.stderr)


def _batch_mode(logfile: str, entries: dict, out_f) -> tuple[int, int]:
    count = 0
    unknown = 0
    with open(logfile, encoding='utf-8', errors='replace') as f:
        for line in f:
            restored = restore_line(line, entries)
            out_f.write(restored + '\n')
            count += 1
            if '[UNKNOWN:' in restored:
                unknown += 1
    return count, unknown


def _follow_mode(logfile: str, entries: dict, out_f):
    print(f"Following {logfile}... (Ctrl+C to stop)", file=sys.stderr)
    with open(logfile, encoding='utf-8', errors='replace') as f:
        while True:
            line = f.readline()
            if not line:
                break
            restored = restore_line(line, entries)
            out_f.write(restored + '\n')
            out_f.flush()

        while True:
            line = f.readline()
            if line:
                restored = restore_line(line, entries)
                out_f.write(restored + '\n')
                out_f.flush()
            else:
                time.sleep(0.1)


if __name__ == '__main__':
    main()

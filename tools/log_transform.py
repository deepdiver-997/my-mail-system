#!/usr/bin/env python3
"""
Build-time log transformation tool.

1. 完整拷贝项目到输出目录 (shutil.copytree)
2. 替换所有 .h/.cpp/.tpp/.hpp 中的 LOG_* 宏为 LOG_PURE(hash, args..., ts)
3. 输出增量映射表
4. 可选自动调用 ./build.sh

用法:
  python tools/log_transform.py                # 变换到 build/transformed/
  python tools/log_transform.py --build        # 变换后自动构建
  python tools/log_transform.py --out /tmp/rel # 自定义输出路径

核心设计:
  - hash = shake128([LEVEL][MODULE]fmt)  — 不含文件/行号, 增删代码 hash 不变
  - 相同模板共享同一 hash — 映射表去重
  - 映射表只追加不删除 — 任意新版映射表可还原所有旧版日志
"""

import os
import re
import sys
import json
import hashlib
import shutil
import argparse
import subprocess
from pathlib import Path

# ============================================================
_MACRO_NAME_RE = re.compile(
    r'\b(LOG_[A-Z_]+?_(?:TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL))\s*\('
)
_MODULE_LEVEL_RE = re.compile(
    r'^LOG_([A-Z_]+?)_(TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL)$'
)
_STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

_SRC_EXTS = {'.h', '.hpp', '.cpp', '.cc', '.cxx', '.c', '.tpp', '.ipp', '.inl'}
_SKIP_FILES = {'logger.h'}  # 不转换 logger.h (宏定义本身)
_TIMESTAMP_FN = 'mail_system::log_pure_timestamp_ms()'

_IGNORE_PATTERNS = shutil.ignore_patterns(
    '.git', '__pycache__', '*.pyc',
    'build', 'cmake-build-*', 'logs',
    '.idea', '.vscode', '.cache', '.DS_Store', 'node_modules',
)

# ============================================================

def compute_hash(level: str, module: str, fmt_str: str) -> str:
    """hash = [LEVEL][MODULE]fmt, 16 位十六进制"""
    payload = f"[{level}][{module}]{fmt_str}"
    return '0x' + hashlib.shake_128(payload.encode()).digest(8).hex()


def extract_macro_calls(text: str) -> list[dict]:
    """提取所有 LOG_* 调用, 返回按位置降序排列的列表。"""

    line_starts = [0]
    for ln in text.splitlines(keepends=True):
        line_starts.append(line_starts[-1] + len(ln))

    def _line(off):
        for i in range(len(line_starts) - 1):
            if line_starts[i] <= off < line_starts[i + 1]:
                return i + 1
        return len(line_starts) - 1

    results = []
    for m in _MACRO_NAME_RE.finditer(text):
        macro = m.group(1)
        mm = _MODULE_LEVEL_RE.match(macro)
        if not mm:
            continue
        module, level = mm.group(1), mm.group(2)

        # 数括号找闭括号
        paren_start = m.end() - 1
        i, depth = paren_start + 1, 1
        while i < len(text) and depth > 0:
            if text[i] == '(': depth += 1
            elif text[i] == ')': depth -= 1
            i += 1
        if depth != 0:
            continue
        paren_end = i
        call_body = text[paren_start + 1:paren_end - 1]

        # 提取格式字符串
        sm = _STR_RE.search(call_body)
        if not sm:
            continue
        import codecs
        try:
            fmt_str = codecs.decode(sm.group(1), 'unicode_escape')
        except Exception:
            fmt_str = sm.group(1)

        # 提取参数部分
        args_start = sm.end()
        while args_start < len(call_body) and call_body[args_start] in (' ', '\t', '\n'):
            args_start += 1
        if args_start < len(call_body) and call_body[args_start] == ',':
            args_start += 1
        args_str = call_body[args_start:].strip()

        results.append({
            'start': m.start(),
            'end': paren_end,
            'module': module,
            'level': level,
            'fmt_str': fmt_str,
            'args_str': args_str,
            'line': _line(m.start()),
        })

    results.sort(key=lambda c: c['start'], reverse=True)
    return results


def transform_text(text: str, file_rel: str, map_data: dict) -> tuple[str, int]:
    """替换文本中的 LOG_* 为 LOG_PURE。"""
    calls = extract_macro_calls(text)
    if not calls:
        return text, 0

    for call in calls:
        h = compute_hash(call['level'], call['module'], call['fmt_str'])

        if h not in map_data['entries']:
            map_data['entries'][h] = {
                'fmt': call['fmt_str'],
                'level': call['level'],
                'module': call['module'],
                'call_sites': [],
            }
        site = f"{file_rel}:{call['line']}"
        if site not in map_data['entries'][h]['call_sites']:
            map_data['entries'][h]['call_sites'].append(site)

        if call['args_str']:
            replacement = f'LOG_PURE({h}, {call["args_str"]}, {_TIMESTAMP_FN})'
        else:
            replacement = f'LOG_PURE({h}, {_TIMESTAMP_FN})'

        text = text[:call['start']] + replacement + text[call['end']:]

    return text, len(calls)


def load_map(path: str) -> dict:
    if os.path.isdir(path):
        print(f"ERROR: --map must be a file path, got a directory: {path}", file=sys.stderr)
        print(f"  Tip: try --map {os.path.join(path, 'pure_map.json')}", file=sys.stderr)
        sys.exit(1)
    if os.path.exists(path):
        with open(path) as f:
            return json.load(f)
    return {'version': 1, 'entries': {}}


def save_map(path: str, data: dict):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    for v in data['entries'].values():
        v['call_sites'].sort()
    with open(path, 'w') as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def main():
    parser = argparse.ArgumentParser(description='Transform LOG_* to LOG_PURE')
    parser.add_argument('project_dir', nargs='?', default='.',
                        help='项目根目录 (默认: 当前目录)')
    parser.add_argument('--out', default=None,
                        help='输出目录 (默认: <project>/build/transformed)')
    parser.add_argument('--map', default=None,
                        help='映射表路径 (默认: <project>/logs/pure_map.json)')
    parser.add_argument('--build', action='store_true',
                        help='变换后执行 ./build.sh')
    args = parser.parse_args()

    proj = os.path.abspath(args.project_dir)
    dst = args.out or os.path.join(proj, 'build', 'transformed')
    map_path = args.map or os.path.join(proj, 'logs', 'pure_map.json')

    # ---- 1. 全量拷贝 ----
    print(f"Copying project: {proj} → {dst}")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(proj, dst, ignore=_IGNORE_PATTERNS, symlinks=False,
                    dirs_exist_ok=True)
    # copytree 会跳过 logs 目录 (在 ignore 列表里), 但运行时需要写入 pure.log
    os.makedirs(os.path.join(dst, 'logs'), exist_ok=True)

    # ---- 2. 加载映射表 ----
    mdata = load_map(map_path)
    prev = len(mdata['entries'])
    print(f"Mapping table: {prev} entries")

    # ---- 3. 扫描并变换源文件 ----
    stats = {'files': 0, 'replaced': 0}
    for root, dirs, files in os.walk(dst):
        # 跳过 build 目录
        dirs[:] = [d for d in dirs if not d.startswith('build')]

        for fname in files:
            ext = os.path.splitext(fname)[1].lower()
            if ext not in _SRC_EXTS:
                continue
            if fname in _SKIP_FILES:
                continue

            fpath = os.path.join(root, fname)
            try:
                with open(fpath, encoding='utf-8', errors='replace') as f:
                    text = f.read()
            except Exception:
                continue

            rel = os.path.relpath(fpath, dst)
            new_text, n = transform_text(text, rel, mdata)

            if n > 0:
                with open(fpath, 'w', encoding='utf-8') as f:
                    f.write(new_text)
                stats['files'] += 1
                stats['replaced'] += n

    # ---- 4. 保存映射表 ----
    mdata['version'] = mdata.get('version', 1) + 1
    new_entries = len(mdata['entries']) - prev
    save_map(map_path, mdata)

    print(f"\n=== Transform complete ===")
    print(f"  Files modified:  {stats['files']}")
    print(f"  Macros replaced: {stats['replaced']}")
    print(f"  Unique hashes:   {len(mdata['entries'])} (+{new_entries} new)")
    print(f"  Map:             {map_path}")
    print(f"  Output:          {dst}")

    # ---- 5. 可选构建 ----
    if args.build:
        print("\n=== Building ===")
        build_script = os.path.join(dst, 'build.sh')
        if os.path.exists(build_script):
            subprocess.run(['bash', 'build.sh'], cwd=dst)
        else:
            print("ERROR: build.sh not found", file=sys.stderr)
            sys.exit(1)


if __name__ == '__main__':
    main()

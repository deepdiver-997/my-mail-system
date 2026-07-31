#!/usr/bin/env python3
"""
Build-time log transformation tool (v2 — gcc -E 预处理器解析).

1. 全量拷贝项目到输出目录
2. 对每个 .cpp/.h 文件运行 gcc -E -DLOG_LOOK_UP
3. 解析预处理器输出中的 LUK\001 标记和 #line 指令
4. 计算 hash, 替换源文件中 LOG_* → LOG_PURE(hash, args..., ts)
5. 生成嵌入映射表 pure_map_data.h + 增量 JSON 映射表

用法:
  python tools/log_transform.py                  # 变换到 build/transformed/
  python tools/log_transform.py --build          # 变换后自动 ./build.sh
  python tools/log_transform.py --out /tmp/rel   # 自定义输出路径
  python tools/log_transform.py -I /opt/my/include  # 额外 include 路径
"""

import os, re, sys, json, hashlib, shutil, argparse, subprocess, tempfile
from pathlib import Path

# ============================================================
_SRC_EXTS = {'.h', '.hpp', '.cpp', '.cc', '.cxx', '.c', '.tpp', '.ipp', '.inl'}
_SKIP_FILES = {'logger.h'}  # 不转换 logger.h (宏定义自身)
_TIMESTAMP_FN = 'pr::log_pure_timestamp_ms()'

_IGNORE_PATTERNS = shutil.ignore_patterns(
    '.git', '__pycache__', '*.pyc', '*.o', '*.a', '*.so', '*.dylib',
    'build', 'cmake-build-*', 'logs', '*.log',
    '.idea', '.vscode', '.cache', '.DS_Store', 'node_modules',
)

# 在预处理器输出中识别标记行
# 格式: (void)("LUK" "\001" "MODULE" "\001" "LEVEL" "\001" "format...");
_MARKER_RE = re.compile(
    r'\(void\)\("LUK"\s*"\\001"\s*"([^"]+)"\s*"\\001"\s*"([^"]+)"\s*"\\001"\s*"((?:[^"\\]|\\.)*)"\)'
)
# 追踪 #line 指令: # 行号 "文件路径"
_LINE_RE = re.compile(r'^#\s+(\d+)\s+"([^"]+)"')

# Phase 2 使用: 匹配源文件中的 LOG_* 宏调用
_MACRO_NAME_RE = re.compile(
    r'\b(LOG_[A-Z_]+?_(?:TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL))\s*\('
)
_MODULE_LEVEL_RE = re.compile(
    r'^LOG_([A-Z_]+?)_(TRACE|DEBUG|INFO|WARN|ERROR|CRITICAL)$'
)
_STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# ============================================================

def discover_include_paths(extra: list[str] = None) -> list[str]:
    """自动发现系统 include 路径。"""
    paths = []
    for p in ['/opt/homebrew/include', '/usr/local/include']:
        if os.path.isdir(p):
            paths.append(p)
    if extra:
        paths.extend(extra)
    return paths


def compute_hash(level: str, module: str, fmt_str: str) -> str:
    """hash = shake128([LEVEL][MODULE]fmt) → 16 位 hex + 0x"""
    payload = f"[{level}][{module}]{fmt_str}"
    return '0x' + hashlib.shake_128(payload.encode()).digest(8).hex()


def preprocess_and_extract(filepath: str, include_dirs: list[str]) -> list[dict]:
    """gcc -E -DLOG_LOOK_UP，返回该文件中**所有**标记 (含被 include 的头文件)。

    返回 [{file, line, module, level, fmt_str}, ...]
    file 字段是 #line 指令指向的真实源文件。
    """
    include_flags = []
    for d in include_dirs:
        include_flags.extend(['-I', d])

    cmd = ['g++', '-E', '-DLOG_LOOK_UP'] + include_flags + [filepath]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        print("ERROR: g++ not found.", file=sys.stderr)
        sys.exit(1)

    output = proc.stdout
    if proc.returncode != 0 and not output:
        print(f"WARNING: gcc -E failed for {filepath}", file=sys.stderr)
        return []

    results = []
    current_file = filepath
    current_line = 0

    for line in output.splitlines():
        lm = _LINE_RE.match(line.strip())
        if lm:
            current_line = int(lm.group(1)) - 1
            current_file = lm.group(2)
            # 规范化路径
            if not os.path.isabs(current_file):
                current_file = os.path.normpath(
                    os.path.join(os.path.dirname(filepath), current_file))
            continue

        current_line += 1

        mm = _MARKER_RE.search(line)
        if not mm:
            continue

        module, level, fmt_str = mm.group(1), mm.group(2), mm.group(3)
        import codecs
        try:
            fmt_str = codecs.decode(fmt_str, 'unicode_escape')
        except Exception:
            pass
        results.append({
            'file': os.path.abspath(current_file) if os.path.exists(current_file) else current_file,
            'line': current_line,
            'module': module,
            'level': level,
            'fmt_str': fmt_str,
        })

    return results


def transform_file(fpath: str, calls: list[dict], map_data: dict) -> tuple[str, int]:
    """替换文件文本——用正则+括号计数正确处理跨行宏。

    gcc -E 分析阶段提供了准确的 (module, level, fmt, line) 四元组。
    这里用正则找到 LOG_* 宏的完整跨度（含跨行）然后替换。
    """
    with open(fpath, encoding='utf-8', errors='replace') as f:
        text = f.read()

    # 按宏名起始位置建立索引 {start_pos: call_info}
    call_index = {}
    for m in _MACRO_NAME_RE.finditer(text):
        macro = m.group(1)
        mm = _MODULE_LEVEL_RE.match(macro)
        if not mm:
            continue
        module, level = mm.group(1), mm.group(2)
        line = text[:m.start()].count('\n') + 1
        call_index[(line, module, level)] = m

    rel = os.path.relpath(fpath)
    replaced = 0

    # 按 start_pos 降序 (从后往前)
    for call in sorted(calls, key=lambda c: c['line'], reverse=True):
        key = (call['line'], call['module'], call['level'])
        cm = call_index.get(key)
        if not cm:
            continue  # gcc -E 分析得到的调用在源码中对不上

        # 括号计数找到宏调用结束位置
        paren_start = cm.end() - 1
        i, depth = paren_start + 1, 1
        while i < len(text) and depth > 0:
            if text[i] == '(': depth += 1
            elif text[i] == ')': depth -= 1
            i += 1
        if depth != 0:
            continue
        paren_end = i

        h = compute_hash(call['level'], call['module'], call['fmt_str'])
        if h not in map_data['entries']:
            map_data['entries'][h] = {
                'fmt': call['fmt_str'],
                'level': call['level'],
                'module': call['module'],
                'call_sites': [],
            }
        site = f"{rel}:{call['line']}"
        if site not in map_data['entries'][h]['call_sites']:
            map_data['entries'][h]['call_sites'].append(site)

        # 提取参数
        call_body = text[paren_start + 1:paren_end - 1]
        fm = _STR_RE.search(call_body)
        if not fm:
            continue
        args_start = fm.end()
        while args_start < len(call_body) and call_body[args_start] in (' ', '\t', '\n'):
            args_start += 1
        if args_start < len(call_body) and call_body[args_start] == ',':
            args_start += 1
        args_str = call_body[args_start:].strip()

        if args_str:
            replacement = f'LOG_PURE({h}, {args_str}, {_TIMESTAMP_FN})'
        else:
            replacement = f'LOG_PURE({h}, {_TIMESTAMP_FN})'

        text = text[:cm.start()] + replacement + text[paren_end:]
        replaced += 1

    if replaced > 0:
        with open(fpath, 'w', encoding='utf-8') as f:
            f.write(text)

    return text, replaced


def load_map(path: str) -> dict:
    if os.path.isdir(path):
        print(f"ERROR: --map must be a file path, got a directory: {path}", file=sys.stderr)
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


def generate_map_header(map_data: dict, dst: str):
    """生成 pure_map_data.h → include/generated/ 可在源码中直接 #include"""
    content = json.dumps(map_data, ensure_ascii=False, separators=(',', ':'))
    header = f'''#pragma once
#include <string_view>
namespace mail_system {{
inline constexpr std::string_view kPureMapData = R"MAP({content})MAP";
}}
'''
    d = os.path.join(dst, 'include', 'generated')
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'pure_map_data.h'), 'w') as f:
        f.write(header)
    # 同时输出 JSON 版本到 logs/ 供外部工具使用
    log_d = os.path.join(dst, 'logs')
    os.makedirs(log_d, exist_ok=True)


def main():
    parser = argparse.ArgumentParser(
        description='Transform LOG_* to LOG_PURE via gcc -E analysis')
    parser.add_argument('project_dir', nargs='?', default='.',
                        help='项目根目录 (默认: 当前目录)')
    parser.add_argument('--out', default=None,
                        help='输出目录 (默认: <project>/build/transformed)')
    parser.add_argument('--map', default=None,
                        help='映射表路径 (默认: <project>/logs/pure_map.json)')
    parser.add_argument('-I', '--include', dest='include_dirs', action='append',
                        default=[], help='额外 include 目录 (可多次指定)')
    parser.add_argument('--build', action='store_true',
                        help='变换后执行 ./build.sh')
    args = parser.parse_args()

    proj = os.path.abspath(args.project_dir)
    dst = args.out or os.path.join(proj, 'build', 'transformed')
    map_path = args.map or os.path.join(proj, 'logs', 'pure_map.json')

    # 禁止输出到项目目录本身——保护源码不被覆盖
    if os.path.realpath(dst) == os.path.realpath(proj):
        print("ERROR: Refusing to transform in-place.", file=sys.stderr)
        print("  Output directory must differ from the project directory.", file=sys.stderr)
        print(f"  Project: {proj}", file=sys.stderr)
        print(f"  Output:  {dst}", file=sys.stderr)
        print("  Tip: omit --out to use default build/transformed/", file=sys.stderr)
        sys.exit(1)

    # ---- 1. 全量拷贝 ----
    print(f"Copying project: {proj} → {dst}")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(proj, dst, ignore=_IGNORE_PATTERNS, symlinks=False,
                    dirs_exist_ok=True)
    os.makedirs(os.path.join(dst, 'logs'), exist_ok=True)

    # ---- 2. include 路径 ----
    inc_dirs = discover_include_paths(args.include_dirs)
    inc_dirs.insert(0, os.path.join(dst, 'include'))
    print(f"Include paths: {inc_dirs}")

    # ---- 3. 预处理分析 ----
    # 只预处理 .cpp 文件。头文件的标记通过 .cpp 的 #include 链间接发现。
    print(f"\n=== Phase 1: gcc -E analysis ===")
    mdata = load_map(map_path)
    prev = len(mdata['entries'])

    # 收集所有 .cpp 文件
    cpp_files = []
    for root, dirs, files in os.walk(dst):
        dirs[:] = [d for d in dirs if not d.startswith('build')]
        for fname in files:
            ext = os.path.splitext(fname)[1].lower()
            if ext in {'.cpp', '.cc', '.cxx', '.c'} and fname not in _SKIP_FILES:
                cpp_files.append(os.path.join(root, fname))

    # 对每个 .cpp 运行 gcc -E, 按 #line 归属文件收集标记
    # 同一头文件被多个 .cpp include → 同 file:line 只保留一次
    from collections import defaultdict
    all_calls: dict[str, list[dict]] = defaultdict(list)
    seen_keys = set()  # (file, line) 去重

    for cpp_path in sorted(cpp_files):
        markers = preprocess_and_extract(cpp_path, inc_dirs)
        for m in markers:
            key = (m['file'], m['line'])
            if key in seen_keys:
                continue
            seen_keys.add(key)
            all_calls[m['file']].append(m)

    total = sum(len(v) for v in all_calls.values())
    # 按调用量排序展示
    for f in sorted(all_calls.keys(), key=lambda x: len(all_calls[x]), reverse=True):
        rel = os.path.relpath(f, dst) if f.startswith(dst) else f
        print(f"  {rel}: {len(all_calls[f])} calls")
    print(f"  Total: {len(all_calls)} files, {total} log calls")

    # ---- 4. 变换阶段 ----
    print(f"\n=== Phase 2: Transform ===")
    stats = {'files': 0, 'replaced': 0}

    for fpath, calls in all_calls.items():
        if not os.path.exists(fpath) or not fpath.startswith(dst):
            continue  # 跳过系统头文件或不存在
        if os.path.basename(fpath) in _SKIP_FILES:
            continue

        _, n = transform_file(fpath, calls, mdata)
        if n > 0:
            stats['files'] += 1
            stats['replaced'] += n

    # ---- 5. 生成映射表 ----
    mdata['version'] = mdata.get('version', 1) + 1
    new_entries = len(mdata['entries']) - prev
    save_map(map_path, mdata)
    generate_map_header(mdata, dst)

    print(f"\n=== Transform complete ===")
    print(f"  Files modified:  {stats['files']}")
    print(f"  Macros replaced: {stats['replaced']}")
    print(f"  Unique hashes:   {len(mdata['entries'])} (+{new_entries} new)")
    print(f"  Map (JSON):      {map_path}")
    print(f"  Map (header):    {os.path.join(dst, 'generated', 'pure_map_data.h')}")
    print(f"  Output:          {dst}")

    # ---- 6. 可选构建 ----
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

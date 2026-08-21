#!/usr/bin/env bash
# Link helper for object-only builds.
# Example:
#   ./link.sh smtps_test.cpp.o
#   ./link.sh smtp_test.o -o smtpsServer

set -euo pipefail

SCRIPT_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

print_usage() {
    cat <<'EOF'
Usage: ./link.sh <entry_obj> [-o output] [--obj-root dir] [--compiler cxx] [--dry-run]

Options:
  --obj-root <dir>   显式指定收集 .o 的根目录。强烈建议在部署脚本里always 指定：
                     不指定时会退回按脚本所在位置推断，若同一目录下存在多份
                     对象树（如 obj/ 与 build-obj/）会被一并链接 → 重复符号。
  --exclude <pat>    跳过路径含该子串的 .o（用于排除其它入口的 main）
  --compiler <cxx>   指定编译器（交叉部署时通常是目标机上的 g++-13）

Examples:
  ./link.sh smtps_test.cpp.o
  ./link.sh smtp_test.o -o smtpsServer
  ./link.sh smtps_test.cpp.o --compiler g++-13
  ./link.sh obj/CMakeFiles/smtpsServer_obj.dir/test/smtps_test.cpp.o \
      --obj-root obj -o smtpsServer --compiler g++-13
    ./link.sh smtps_test.cpp.o --pie
  ./link.sh smtps_test.cpp.o --dry-run
EOF
}

if [[ $# -lt 1 ]]; then
    print_usage
    exit 1
fi

ENTRY_INPUT="$1"
shift

OUTPUT="smtpsServer"
CXX_BIN="${CXX:-g++}"
DRY_RUN="0"
PIE_MODE="OFF"
OBJ_ROOT_OVERRIDE=""
EXCLUDE_PATTERNS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o|--output)
            [[ $# -ge 2 ]] || { echo "Missing value for $1" >&2; exit 1; }
            OUTPUT="$2"
            shift 2
            ;;
        --compiler)
            [[ $# -ge 2 ]] || { echo "Missing value for --compiler" >&2; exit 1; }
            CXX_BIN="$2"
            shift 2
            ;;
        --obj-root)
            [[ $# -ge 2 ]] || { echo "Missing value for --obj-root" >&2; exit 1; }
            OBJ_ROOT_OVERRIDE="$2"
            shift 2
            ;;
        --exclude)
            [[ $# -ge 2 ]] || { echo "Missing value for --exclude" >&2; exit 1; }
            EXCLUDE_PATTERNS+=("$2")
            shift 2
            ;;
        --dry-run)
            DRY_RUN="1"
            shift
            ;;
        --pie)
            PIE_MODE="ON"
            shift
            ;;
        -h|--help)
            print_usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            print_usage
            exit 1
            ;;
    esac
done

if ! command -v "$CXX_BIN" >/dev/null 2>&1; then
    echo "C++ compiler not found: $CXX_BIN" >&2
    exit 1
fi

resolve_entry_obj() {
    local input="$1"
    local base stem

    if [[ -f "$input" ]]; then
        printf '%s\n' "$input"
        return
    fi

    base="$(basename "$input")"
    stem="${base%.o}"

    local matches=()
    while IFS= read -r line; do
        matches+=("$line")
    done < <(find . -type f -name "$base" | sort)
    if [[ ${#matches[@]} -eq 0 ]]; then
        while IFS= read -r line; do
            matches+=("$line")
        done < <(find . -type f -name "*${stem}*.o" | sort)
    fi
    if [[ ${#matches[@]} -eq 0 ]]; then
        local wildcard_stem
        wildcard_stem="${stem//_/*}"
        wildcard_stem="${wildcard_stem//-/*}"
        while IFS= read -r line; do
            matches+=("$line")
        done < <(find . -type f -name "*${wildcard_stem}*.o" | sort)
    fi

    if [[ ${#matches[@]} -eq 0 ]]; then
        echo "Cannot find entry object: $input" >&2
        exit 1
    fi

    if [[ ${#matches[@]} -gt 1 ]]; then
        echo "Multiple matching object files found for '$input', using first:" >&2
        printf '  %s\n' "${matches[@]}" >&2
    fi

    printf '%s\n' "${matches[0]}"
}

ENTRY_OBJ="$(resolve_entry_obj "$ENTRY_INPUT")"

abspath() {
    local p="$1"
    local d
    if [[ -d "$p" ]]; then
        (cd "$p" && pwd)
    else
        d="$(cd "$(dirname "$p")" && pwd)"
        printf '%s/%s\n' "$d" "$(basename "$p")"
    fi
}

ENTRY_OBJ_ABS="$(abspath "$ENTRY_OBJ")"

# 对象根目录：优先用显式 --obj-root。
#
# 没有显式指定时才退回推断，但推断规则有个历史陷阱：入口 .o 若位于脚本所在
# 目录之下，就会收集该目录下**所有** .o。部署机上同时存在 obj/ 与 build-obj/
# 两份对象树时，两份会被一起链接 → 重复符号。过去靠"把 link.sh 拷到 /tmp 再跑"
# 来绕开，属于用脚本位置来控制行为，太隐晦。部署脚本请一律显式传 --obj-root。
if [[ -n "$OBJ_ROOT_OVERRIDE" ]]; then
    if [[ ! -d "$OBJ_ROOT_OVERRIDE" ]]; then
        echo "Object root not found: $OBJ_ROOT_OVERRIDE" >&2
        exit 1
    fi
    OBJ_ROOT="$(abspath "$OBJ_ROOT_OVERRIDE")"
    if [[ "$ENTRY_OBJ_ABS" != "$OBJ_ROOT/"* ]]; then
        echo "Entry object is outside --obj-root:" >&2
        echo "  entry    : $ENTRY_OBJ_ABS" >&2
        echo "  obj-root : $OBJ_ROOT" >&2
        exit 1
    fi
elif [[ "$ENTRY_OBJ_ABS" == "$SCRIPT_HOME/"* ]]; then
    OBJ_ROOT="$SCRIPT_HOME"
elif [[ "$ENTRY_OBJ_ABS" == *".dir/"* ]]; then
    OBJ_ROOT="${ENTRY_OBJ_ABS%%.dir/*}.dir"
else
    OBJ_ROOT="$(dirname "$ENTRY_OBJ_ABS")"
fi

if [[ ! -d "$OBJ_ROOT" ]]; then
    echo "Object root not found: $OBJ_ROOT" >&2
    exit 1
fi

# 同一份对象树里出现多个 CMake 对象目录时提前告警：它们各自带一个 main，
# 链接必然重复符号。历史上 obj/ 与 build-obj/ 混链就是这么炸的。
_obj_dirs="$(find "$OBJ_ROOT" -type d -name '*_obj.dir' 2>/dev/null | wc -l | tr -d ' ')"
if [[ "$_obj_dirs" -gt 1 && ${#EXCLUDE_PATTERNS[@]} -eq 0 ]]; then
    echo "Warning: found $_obj_dirs *_obj.dir trees under $OBJ_ROOT and no --exclude given." >&2
    echo "         Multiple entry points will collide (duplicate main). Consider --exclude." >&2
fi

OBJECTS=()
while IFS= read -r line; do
    skip=0
    for pat in "${EXCLUDE_PATTERNS[@]}"; do
        if [[ "$line" == *"$pat"* ]]; then
            skip=1
            break
        fi
    done
    [[ $skip -eq 0 ]] && OBJECTS+=("$line")
done < <(find "$OBJ_ROOT" -type f -name '*.o' | sort)
if [[ ${#OBJECTS[@]} -eq 0 ]]; then
    echo "No object files found under: $OBJ_ROOT" >&2
    exit 1
fi

# Ensure entry object is present in the link list.
if ! printf '%s\n' "${OBJECTS[@]}" | grep -F -x -q "$ENTRY_OBJ_ABS"; then
    OBJECTS+=("$ENTRY_OBJ_ABS")
fi

add_pkg_libs() {
    local pkg="$1"
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists "$pkg"; then
        # shellcheck disable=SC2207
        PKG_LIBS+=( $(pkg-config --libs "$pkg") )
        return 0
    fi
    return 1
}

PKG_LIBS=()
FALLBACK_LIBS=()

# Prefer pkg-config when available.
add_pkg_libs spdlog || FALLBACK_LIBS+=( -lspdlog )
add_pkg_libs openssl || FALLBACK_LIBS+=( -lssl -lcrypto )
add_pkg_libs libcares || add_pkg_libs cares || FALLBACK_LIBS+=( -lcares )
add_pkg_libs mysqlclient || add_pkg_libs mariadb || add_pkg_libs libmariadb || FALLBACK_LIBS+=( -lmysqlclient )

# nlohmann/json and Boost.Asio are header-first, but Boost.System/Thread are commonly required.
FALLBACK_LIBS+=( -lboost_system -lboost_thread -pthread -ldl -lz )

LINK_CMD=( "$CXX_BIN" -o "$OUTPUT" )
if [[ "$PIE_MODE" != "ON" ]]; then
    # Ubuntu enables PIE by default; many transferred .o files are non-PIE.
    LINK_CMD+=( -no-pie )
fi
LINK_CMD+=( "${OBJECTS[@]}" )
LINK_CMD+=( "${PKG_LIBS[@]}" )
LINK_CMD+=( "${FALLBACK_LIBS[@]}" )

echo "Entry object : $ENTRY_OBJ_ABS"
echo "Object root  : $OBJ_ROOT"
echo "Object count : ${#OBJECTS[@]}"
echo "Output file  : $OUTPUT"
echo "Compiler     : $CXX_BIN"
echo ""
echo "Link command:"
printf '  %q' "${LINK_CMD[@]}"
echo ""

if [[ "$DRY_RUN" == "1" ]]; then
    echo ""
    echo "Dry-run only. No linking performed."
    exit 0
fi

if "${LINK_CMD[@]}"; then
    echo ""
    echo "Link succeeded: $OUTPUT"
else
    echo ""
    echo "Link failed."
    echo "Tips:"
    echo "  1) Install target libraries on this machine (boost, spdlog, openssl, c-ares, mysqlclient)."
    echo "  2) Or pass --compiler to use the intended target compiler toolchain."
    exit 1
fi

#!/usr/bin/env bash
# Link helper for object-only builds.
# Example:
#   ./link.sh smtps_test.cpp.o
#   ./link.sh smtp_test.o -o smtpsServer

set -euo pipefail

SCRIPT_HOME="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

print_usage() {
    cat <<'EOF'
Usage: ./link.sh <entry_obj> [-o output] [--compiler cxx] [--dry-run]

Examples:
  ./link.sh smtps_test.cpp.o
  ./link.sh smtp_test.o -o smtpsServer
  ./link.sh smtps_test.cpp.o --compiler g++-13
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

# For exported artifacts, collect all .o files under script directory.
# For local CMake build, collect objects from the single *.dir directory.
if [[ "$ENTRY_OBJ_ABS" == "$SCRIPT_HOME/"* ]]; then
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

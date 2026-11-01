#!/usr/bin/env bash
# launch_wine.sh - run the caster under Wine or Proton after checking the
# environment. Usage: bash launch_wine.sh [--check|--recheck|--strict]
# [--game-dir DIR] [-- caster arguments]. The only file it writes is the
# .qoh99-preflight marker beside the game.
set -euo pipefail

SCRIPT_VERSION=2
MARKER_NAME='.qoh99-preflight'

SUPPORTED_GAME_SHA256="a79002592953e8d9ace3e363b3115ce4452c1a0789eb5b17c9bcb032af27f1f6"
SUPPORTED_GAME_SIZE="704512"
SUPPORTED_GAME_TIMESTAMP="0x3a19a8a9"
SUPPORTED_GAME_KEY="se_2006"
OTHER_GAME_SHA256="c41e755e6609b39749716fee02d3a393ef8a11f9d8bf84419aa9131b5e91eb63"
OTHER_GAME_KEY="game_2000"

PINNED_SDL3_SHA256="c68977dfc08f64b7bc845d04bbb01eb534f4ad52f80d7fa126fb4731185bcb63"

REQUIRED_WRAPPER_EXPORTS="EFZDDRAW_GetNetplayPacingAbi EFZDDRAW_SetFrameLimit"

CONTRACT_PREFIX="QOH99_NETPLAY_CONTRACT_"

PACKAGE_RUNTIME_FILES="qoh99caster.exe qoh99_netplay.dll ddraw.dll SDL3.dll
launch_wine.sh"

PACKAGE_DOC_FILES="README.txt"

PACKAGE_BINARIES="qoh99caster.exe qoh99_netplay.dll ddraw.dll SDL3.dll"

KNOWN_SYSTEM_IMPORTS="kernel32.dll user32.dll gdi32.dll advapi32.dll
ws2_32.dll wininet.dll dinput8.dll dinput.dll iphlpapi.dll bcrypt.dll
dbghelp.dll winmm.dll ole32.dll oleaut32.dll shell32.dll shlwapi.dll
imm32.dll d3d9.dll d3d11.dll dxgi.dll version.dll setupapi.dll cfgmgr32.dll
hid.dll rpcrt4.dll comdlg32.dll crypt32.dll ntdll.dll psapi.dll powrprof.dll
winspool.drv usp10.dll uxtheme.dll dwmapi.dll avrt.dll"

PACKAGE_PROVIDED_IMPORTS="sdl3.dll ddraw.dll"

FORBIDDEN_CRT_PATTERN='^(msvcr|msvcp|vcruntime|ucrtbase|api-ms-win-crt)'

LEGACY_ASSET_PATH_MAX=127
LEGACY_ASSET_PATH_WARN=96

DEFAULT_GAME_PORT=7500

FAILURES=0
WARNINGS=0
SKIPS=0
VERBOSE=0

section() { printf '\n== %s ==\n' "$1"; }
ok()      { printf '  [ ok ] %s\n' "$1"; }
warn()    { WARNINGS=$((WARNINGS + 1)); printf '  [warn] %s\n' "$1"; }
bad()     { FAILURES=$((FAILURES + 1)); printf '  [FAIL] %s\n' "$1"; }
skip()    { SKIPS=$((SKIPS + 1)); printf '  [skip] %s\n' "$1"; }
note()    { printf '         %s\n' "$1"; }
vnote()   { if [ "$VERBOSE" = "1" ]; then printf '         %s\n' "$1"; fi; }

usage() {
    cat <<'USAGE'
qoh99 netplay - Wine/Proton preflight

  bash launch_wine.sh --game-dir /path/to/QOH99 [options]

  --game-dir <path>    REQUIRED. Directory holding the supported game
                       executable and its System/ data.
  --wine <binary>      Wine binary to interrogate (default: $WINE, else the
                       first `wine` on PATH).
  --prefix <path>      WINEPREFIX to report on (default: $WINEPREFIX, else
                       ~/.wine). The prefix is never modified.
  --relay <host:port>  Also check relay address resolution/connectivity.
  --verbose            Print per-item detail (imports, exports, mounts).
  --help               This text.

Exit status: 0 = no hard failure, 1 = hard failure, 2 = usage error.

Preflight success is necessary but NEVER sufficient: it cannot prove D3D,
controller, audio, gameplay speed, rollback, spectator or rematch behaviour.
USAGE
}

die_usage() {
    printf 'launch_wine.sh: %s\n\n' "$1" >&2
    usage >&2
    exit 2
}

have() { command -v "$1" >/dev/null 2>&1; }

lower() { printf '%s' "$1" | tr '[:upper:]' '[:lower:]'; }

abspath() {
    if have readlink && readlink -f "$1" >/dev/null 2>&1; then
        readlink -f "$1"
    elif [ -d "$1" ]; then
        (cd "$1" 2>/dev/null && pwd)
    else
        case "$1" in
            /*) printf '%s' "$1" ;;
            *)  printf '%s/%s' "$(pwd)" "$1" ;;
        esac
    fi
}

sha256_of() {
    if have sha256sum; then
        sha256sum "$1" | cut -d' ' -f1
    elif have shasum; then
        shasum -a 256 "$1" | cut -d' ' -f1
    elif have openssl; then
        openssl dgst -sha256 "$1" | sed 's/.*= //'
    else
        return 1
    fi
}

size_of() { wc -c < "$1" | tr -d ' \n'; }

binary_marker() { # file prefix
    LC_ALL=C grep -a -o -m1 "$2[A-Za-z0-9_]*" "$1" 2>/dev/null || true
}

is_ascii() {
    case "$1" in
        *[!\ -~]*) return 1 ;;
        *) return 0 ;;
    esac
}

pe_hex() { # file offset count -> lowercase hex, no separators
    od -An -tx1 -v -j "$2" -N "$3" "$1" 2>/dev/null | tr -d ' \n\t'
}

pe_u() { # file offset size -> little-endian unsigned decimal
    local hex value index byte
    hex=$(pe_hex "$1" "$2" "$3")
    [ "${#hex}" -eq $(( $3 * 2 )) ] || return 1
    value=0
    index=$(( $3 - 1 ))
    while [ "$index" -ge 0 ]; do
        byte=${hex:$((index * 2)):2}
        value=$(( value * 256 + 16#$byte ))
        index=$((index - 1))
    done
    printf '%s' "$value"
}

pe_cstring() { # file offset [max]
    local hex escaped index byte
    hex=$(pe_hex "$1" "$2" "${3:-96}")
    escaped=""
    index=0
    while [ "$index" -lt "${#hex}" ]; do
        byte=${hex:$index:2}
        [ "$byte" = "00" ] && break
        escaped="$escaped\\x$byte"
        index=$((index + 2))
    done
    printf "$escaped"
}

pe_lfanew() { # file -> PE header offset, or failure
    local mz lfanew signature
    mz=$(pe_hex "$1" 0 2)
    [ "$mz" = "4d5a" ] || return 1
    lfanew=$(pe_u "$1" 60 4) || return 1
    [ "$lfanew" -gt 0 ] 2>/dev/null || return 1
    signature=$(pe_hex "$1" "$lfanew" 4)
    [ "$signature" = "50450000" ] || return 1
    printf '%s' "$lfanew"
}

pe_machine()      { pe_u "$1" $(( $2 + 4 )) 2; }
pe_timestamp()    { pe_u "$1" $(( $2 + 8 )) 4; }
pe_sections()     { pe_u "$1" $(( $2 + 6 )) 2; }
pe_opt_size()     { pe_u "$1" $(( $2 + 20 )) 2; }
pe_opt_magic()    { pe_u "$1" $(( $2 + 24 )) 2; }
pe_image_base()   { pe_u "$1" $(( $2 + 24 + 28 )) 4; }

pe_dir_rva() { # file lfanew index
    pe_u "$1" $(( $2 + 24 + 96 + $3 * 8 )) 4
}

pe_rva_to_offset() { # file lfanew rva -> file offset
    local count opt base index sec va raw ptr span
    count=$(pe_sections "$1" "$2") || return 1
    opt=$(pe_opt_size "$1" "$2") || return 1
    base=$(( $2 + 24 + opt ))
    index=0
    while [ "$index" -lt "$count" ]; do
        sec=$(( base + index * 40 ))
        va=$(pe_u "$1" $(( sec + 12 )) 4) || return 1
        raw=$(pe_u "$1" $(( sec + 16 )) 4) || return 1
        ptr=$(pe_u "$1" $(( sec + 20 )) 4) || return 1
        span=$raw
        if [ "$span" -eq 0 ]; then
            span=$(pe_u "$1" $(( sec + 8 )) 4) || return 1
        fi
        if [ "$3" -ge "$va" ] && [ "$3" -lt $(( va + span )) ]; then
            printf '%s' $(( ptr + $3 - va ))
            return 0
        fi
        index=$((index + 1))
    done
    return 1
}

pe_import_names() { # file lfanew
    local rva offset name entry seen=" "
    rva=$(pe_dir_rva "$1" "$2" 1) || return 1
    if [ "$rva" -gt 0 ]; then
        offset=$(pe_rva_to_offset "$1" "$2" "$rva") || return 1
        entry=$offset
        while :; do
            local name_rva name_offset
            name_rva=$(pe_u "$1" $(( entry + 12 )) 4) || break
            [ "$name_rva" -eq 0 ] && break
            name_offset=$(pe_rva_to_offset "$1" "$2" "$name_rva") || break
            name=$(lower "$(pe_cstring "$1" "$name_offset")")
            case "$seen" in *" $name "*) : ;; *) seen="$seen$name " ;; esac
            entry=$(( entry + 20 ))
        done
    fi
    rva=$(pe_dir_rva "$1" "$2" 13) || rva=0
    if [ "$rva" -gt 0 ]; then
        local base image_base attributes
        image_base=$(pe_image_base "$1" "$2") || image_base=0
        base=$(pe_rva_to_offset "$1" "$2" "$rva") || base=""
        if [ -n "$base" ]; then
            entry=$base
            while :; do
                local name_rva name_offset
                attributes=$(pe_u "$1" "$entry" 4) || break
                name_rva=$(pe_u "$1" $(( entry + 4 )) 4) || break
                [ "$name_rva" -eq 0 ] && break
                if [ $(( attributes & 1 )) -eq 0 ] \
                   && [ "$name_rva" -gt "$image_base" ]; then
                    name_rva=$(( name_rva - image_base ))
                fi
                name_offset=$(pe_rva_to_offset "$1" "$2" "$name_rva") || break
                name=$(lower "$(pe_cstring "$1" "$name_offset")")
                case "$seen" in
                    *" $name "*) : ;;
                    *) seen="$seen$name " ;;
                esac
                entry=$(( entry + 32 ))
            done
        fi
    fi
    printf '%s' "$seen"
}

pe_exports() { # file lfanew
    local rva dir base count names ordinals index name_rva name_offset
    local ordinal_index
    rva=$(pe_dir_rva "$1" "$2" 0) || return 1
    [ "$rva" -gt 0 ] || return 1
    dir=$(pe_rva_to_offset "$1" "$2" "$rva") || return 1
    base=$(pe_u "$1" $(( dir + 16 )) 4) || return 1
    count=$(pe_u "$1" $(( dir + 24 )) 4) || return 1
    names=$(pe_u "$1" $(( dir + 32 )) 4) || return 1
    ordinals=$(pe_u "$1" $(( dir + 36 )) 4) || return 1
    [ "$count" -gt 0 ] || return 0
    [ "$count" -lt 65536 ] || return 1
    names=$(pe_rva_to_offset "$1" "$2" "$names") || return 1
    ordinals=$(pe_rva_to_offset "$1" "$2" "$ordinals") || return 1
    index=0
    while [ "$index" -lt "$count" ]; do
        name_rva=$(pe_u "$1" $(( names + index * 4 )) 4) || return 1
        name_offset=$(pe_rva_to_offset "$1" "$2" "$name_rva") || return 1
        ordinal_index=$(pe_u "$1" $(( ordinals + index * 2 )) 2) || return 1
        printf '%s %s\n' $(( base + ordinal_index )) \
                         "$(pe_cstring "$1" "$name_offset")"
        index=$((index + 1))
    done
}

merge_ddraw_override() { # current value -> merged value
    local input="$1" result="" rule libs value keep lib
    local IFS_SAVED="$IFS"
    IFS=';'
    for rule in $input; do
        [ -n "$rule" ] || continue
        case "$rule" in
            *=*)
                libs=${rule%%=*}
                value=${rule#*=}
                ;;
            *)
                libs=$rule
                value=""
                ;;
        esac
        keep=""
        local IFS_INNER="$IFS"
        IFS=','
        for lib in $libs; do
            case "$(lower "$lib")" in
                ddraw|ddraw.dll) continue ;;
            esac
            if [ -z "$keep" ]; then keep="$lib"; else keep="$keep,$lib"; fi
        done
        IFS="$IFS_INNER"
        [ -n "$keep" ] || continue
        if [ -z "$result" ]; then
            result="$keep=$value"
        else
            result="$result;$keep=$value"
        fi
    done
    IFS="$IFS_SAVED"
    if [ -z "$result" ]; then
        printf 'ddraw=n,b'
    else
        printf '%s;ddraw=n,b' "$result"
    fi
}

usage() {
    cat <<'USAGE'
Usage: bash launch_wine.sh [options] [-- caster args...]

With no arguments at all, this checks and launches the game sitting beside it.
That is the normal case: the package is extracted into the game folder.

  --game-dir <path>   The directory holding qoh99.exe. Defaults to the folder
                      this script is in, which is where it ships.
  --wine <binary>     Wine binary to use (default: $WINE, else `wine`).
  --prefix <path>     WINEPREFIX to use (default: $WINEPREFIX, else ~/.wine).
  --relay host[:port] Also check that a relay is reachable.
  --check             Run the checks and exit. Does not launch. This is what
                      to run and paste when asking for help.
  --recheck           Ignore the stored marker and check again before
                      launching.
  --skip-preflight    Launch without checking at all.
  --window            Give the caster its OWN console window
                      (wineconsole) instead of drawing in this terminal.
  --in-terminal       Draw in this terminal even if a window was possible.
                      Default is a window when wineconsole and a display are
                      both available, because the caster is a Win32 console
                      program and wine's terminal emulation of one is
                      approximate.
  --no-clear          Do not clear the screen before the caster's menu
                      appears. The menu is a full-screen text UI and starts by
                      drawing over whatever the checks just printed.
  --wine-stderr       Keep wine's err:/fixme:/CRITICAL output on this
                      terminal. By default it goes to
                      logs/wine-stderr.log, because it otherwise draws
                      over the caster's menu.
  --strict            Treat any check failure as fatal and refuse to launch.
                      Off by default: the caster is the authority on package
                      validity and refuses on its own, so an unverified
                      script should not be the thing that stops you playing.
  --verbose, -v       More detail from the checks.
  --help, -h          This text.

QOH99_CONSOLE_VT overrides how the caster draws (1 = VT escape sequences,
0 = console cell writes). It is set automatically to match the console this
script chose; set it yourself only if the menu renders wrongly.

Everything after `--` is passed to qoh99caster.exe unchanged, e.g.
  bash launch_wine.sh --game-dir ~/QOH99 -- host 7500
USAGE
}

die() { printf 'launch_wine.sh: %s\n' "$1" >&2; exit 2; }
die_usage() { printf 'launch_wine.sh: %s\n\n' "$1" >&2; usage >&2; exit 2; }

GAME_DIR=""
WINE_BIN="${WINE:-}"
WINE_PREFIX="${WINEPREFIX:-}"
RELAY_TARGET=""
CHECK_ONLY=0
RECHECK=0
SKIP_PREFLIGHT=0
STRICT=0
WINE_STDERR_TO_TERMINAL=0
NO_CLEAR=0
CONSOLE_MODE=auto

while [ $# -gt 0 ]; do
    case "$1" in
        --game-dir) [ $# -ge 2 ] || die_usage "--game-dir needs a path"
                    GAME_DIR="$2"; shift 2 ;;
        --game-dir=*) GAME_DIR="${1#*=}"; shift ;;
        --wine)     [ $# -ge 2 ] || die_usage "--wine needs a binary"
                    WINE_BIN="$2"; shift 2 ;;
        --wine=*)   WINE_BIN="${1#*=}"; shift ;;
        --prefix)   [ $# -ge 2 ] || die_usage "--prefix needs a path"
                    WINE_PREFIX="$2"; shift 2 ;;
        --prefix=*) WINE_PREFIX="${1#*=}"; shift ;;
        --relay)    [ $# -ge 2 ] || die_usage "--relay needs host:port"
                    RELAY_TARGET="$2"; shift 2 ;;
        --relay=*)  RELAY_TARGET="${1#*=}"; shift ;;
        --check)    CHECK_ONLY=1; shift ;;
        --recheck)  RECHECK=1; shift ;;
        --skip-preflight) SKIP_PREFLIGHT=1; shift ;;
        --strict)   STRICT=1; shift ;;
        --wine-stderr) WINE_STDERR_TO_TERMINAL=1; shift ;;
        --no-clear) NO_CLEAR=1; shift ;;
        --window)   CONSOLE_MODE=window; shift ;;
        --in-terminal) CONSOLE_MODE=terminal; shift ;;
        --verbose|-v) VERBOSE=1; shift ;;
        --help|-h)  usage; exit 0 ;;
        --)         shift; break ;;
        *)          die_usage "unknown option: $1" ;;
    esac
done

SCRIPT_PATH=$(abspath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
PKG_DIR="$SCRIPT_DIR"

if [ -z "$GAME_DIR" ]; then
    GAME_DIR="$SCRIPT_DIR"
    if [ ! -f "$GAME_DIR/qoh99.exe" ]; then
        printf 'launch_wine.sh: no qoh99.exe beside this script.\n\n' >&2
        printf '  this script : %s\n' "$SCRIPT_PATH" >&2
        printf '  looked in   : %s\n\n' "$GAME_DIR" >&2
        printf 'The package is meant to be extracted INTO the game folder, so\n' >&2
        printf 'that this script sits next to qoh99.exe. Either move it there,\n' >&2
        printf 'or name the game folder explicitly:\n\n' >&2
        printf '  bash launch_wine.sh --game-dir "/path/to/QOH99"\n\n' >&2
        exit 2
    fi
fi
[ -d "$GAME_DIR" ] || die_usage "--game-dir is not a directory: $GAME_DIR"
GAME_DIR=$(abspath "$GAME_DIR")
STATE_DIR="${QOH99_NETPLAY_STATE_DIR:-$GAME_DIR}"
CASTER="$PKG_DIR/qoh99caster.exe"
MARKER_PATH="$GAME_DIR/$MARKER_NAME"
WINE_LOG="$GAME_DIR/logs/wine-stderr.log"
CHECK_LOG="$GAME_DIR/logs/preflight.log"

run_checks() {
    printf 'QOH99 netplay Wine/Proton preflight\n'
    printf '  package : %s\n' "$PKG_DIR"
    printf '  game    : %s\n' "$GAME_DIR"
    printf '  state   : %s\n' "$STATE_DIR"
    printf '  date    : %s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)"

    for tool in od dd tr cut sed wc; do
        have "$tool" || bad "required POSIX tool is missing: $tool"
    done

    exact_case_report() { # directory relative-name label [severity: bad|warn]
        local dir="$1" name="$2" label="$3" severity="${4:-bad}" entry candidate wanted
        if [ -e "$dir/$name" ]; then
            return 0
        fi
        wanted=$(lower "$name")
        candidate=""
        for entry in "$dir"/*; do
            [ -e "$entry" ] || continue
            if [ "$(lower "$(basename "$entry")")" = "$wanted" ]; then
                candidate=$(basename "$entry")
                break
            fi
        done
        if [ -n "$candidate" ]; then
            "$severity" "$label: found '$candidate' but Linux needs the exact name '$name'"
            note "rename it: mv '$dir/$candidate' '$dir/$name'"
        else
            "$severity" "$label: missing $dir/$name"
        fi
        return 1
    }

    section "1. package files, game executable, game data"

    package_failures_before=$FAILURES
    package_warnings_before=$WARNINGS
    check_package_group() { # severity file-list
        local severity="$1" list="$2" relative directory name
        local reported_dirs=" "
        for relative in $list; do
            directory="$PKG_DIR"
            name="$relative"
            case "$relative" in
                */*) directory="$PKG_DIR/${relative%/*}"; name="${relative##*/}" ;;
            esac
            if [ ! -d "$directory" ]; then
                case "$reported_dirs" in
                    *" $directory "*) ;;
                    *) "$severity" "package directory is missing: $directory"
                       reported_dirs="$reported_dirs$directory " ;;
                esac
                continue
            fi
            if exact_case_report "$directory" "$name" "package file" "$severity"; then
                vnote "package file present: $relative"
            fi
        done
    }
    check_package_group bad  "$PACKAGE_RUNTIME_FILES"
    check_package_group warn "$PACKAGE_DOC_FILES"
    if [ "$FAILURES" -eq "$package_failures_before" ]; then
        if [ "$WARNINGS" -eq "$package_warnings_before" ]; then
            ok "every packaged file is present with its exact name"
        else
            ok "every file needed to run is present; only documentation is missing"
            note "documentation and licences do not affect the game"
        fi
    fi

    GAME_EXE=""
    for candidate_name in qoh99.exe GAME.EXE; do
        if [ -f "$GAME_DIR/$candidate_name" ]; then
            GAME_EXE="$GAME_DIR/$candidate_name"
            break
        fi
    done
    if [ -z "$GAME_EXE" ]; then
        for entry in "$GAME_DIR"/*; do
            [ -f "$entry" ] || continue
            case "$(lower "$(basename "$entry")")" in
                qoh99.exe|game.exe)
                    bad "game executable case mismatch: found '$(basename "$entry")'"
                    note "Wine matches Windows names case-insensitively, but the "
                    note "game's own asset loader and this package do not."
                    GAME_EXE="$entry"
                    break
                    ;;
            esac
        done
    fi
    if [ -z "$GAME_EXE" ]; then
        bad "no qoh99.exe (or GAME.EXE) in $GAME_DIR"
    else
        ok "game executable: $GAME_EXE"
    fi

    if [ -d "$GAME_DIR/System" ]; then
        ok "game data directory present: System/"
        if [ -f "$GAME_DIR/System/QOHcnf.key" ]; then
            ok "game configuration present: System/QOHcnf.key"
        else
            bad "System/QOHcnf.key is missing; the install is incomplete"
        fi
    else
        exact_case_report "$GAME_DIR" "System" "game data directory" || true
    fi
    if [ -d "$GAME_DIR/LocalConfig" ]; then
        vnote "LocalConfig/ present (Extra functions / FCommand.txt)"
    else
        warn "LocalConfig/ is absent; Settings -> Extra functions will create it"
    fi

    section "2. supported game build"

    if [ -n "$GAME_EXE" ] && [ -f "$GAME_EXE" ]; then
        if game_sha=$(sha256_of "$GAME_EXE"); then
            game_sha=$(lower "$game_sha")
            if [ "$game_sha" = "$SUPPORTED_GAME_SHA256" ]; then
                ok "game SHA-256 matches the supported build '$SUPPORTED_GAME_KEY'"
            elif [ "$game_sha" = "$OTHER_GAME_SHA256" ]; then
                bad "recognized build '$OTHER_GAME_KEY', but the production hooks"
                note "require '$SUPPORTED_GAME_KEY' (qoh99.exe, SE 2006)."
            else
                bad "unsupported game executable: SHA-256=$game_sha"
                note "expected $SUPPORTED_GAME_SHA256"
            fi
        else
            skip "no sha256sum/shasum/openssl; cannot hash the game executable"
        fi

        game_size=$(size_of "$GAME_EXE")
        if [ "$game_size" = "$SUPPORTED_GAME_SIZE" ]; then
            ok "game size $game_size bytes"
        else
            bad "game size $game_size (expected $SUPPORTED_GAME_SIZE)"
        fi

        if game_lfanew=$(pe_lfanew "$GAME_EXE"); then
            game_machine=$(pe_machine "$GAME_EXE" "$game_lfanew")
            game_stamp=$(pe_timestamp "$GAME_EXE" "$game_lfanew")
            game_stamp_hex=$(printf '0x%08x' "$game_stamp")
            if [ "$game_machine" -eq 332 ]; then
                ok "game PE machine 0x014c (i386)"
            else
                bad "game PE machine $(printf '0x%04x' "$game_machine") is not i386"
            fi
            if [ "$game_stamp_hex" = "$SUPPORTED_GAME_TIMESTAMP" ]; then
                ok "game COFF timestamp $game_stamp_hex"
            else
                bad "game COFF timestamp $game_stamp_hex (expected $SUPPORTED_GAME_TIMESTAMP)"
            fi
        else
            bad "game executable is not a readable PE image"
        fi
    else
        skip "no game executable to inspect"
    fi

    section "3. shipped binaries: PE32/i386 and import closure"

    for binary in $PACKAGE_BINARIES; do
        path="$PKG_DIR/$binary"
        if [ ! -f "$path" ]; then
            bad "$binary is missing; cannot check its architecture"
            continue
        fi
        if ! lfanew=$(pe_lfanew "$path"); then
            bad "$binary is not a readable PE image"
            continue
        fi
        machine=$(pe_machine "$path" "$lfanew")
        magic=$(pe_opt_magic "$path" "$lfanew")
        if [ "$machine" -ne 332 ]; then
            bad "$binary PE machine $(printf '0x%04x' "$machine") is not i386 (0x014c)"
            continue
        fi
        if [ "$magic" -ne 267 ]; then
            bad "$binary is PE32+ (64-bit); the game process is 32-bit"
            continue
        fi
        ok "$binary is PE32/i386"

        if imports=$(pe_import_names "$path" "$lfanew"); then
            for dll in $imports; do
                vnote "$binary imports $dll"
                case " $PACKAGE_PROVIDED_IMPORTS " in
                    *" $dll "*)
                        if [ ! -f "$PKG_DIR/$dll" ]; then
                            found=""
                            for entry in "$PKG_DIR"/*; do
                                [ -f "$entry" ] || continue
                                if [ "$(lower "$(basename "$entry")")" = "$dll" ]; then
                                    found=$(basename "$entry")
                                    break
                                fi
                            done
                            if [ -z "$found" ]; then
                                bad "$binary imports $dll, which the package does not ship"
                            fi
                        fi
                        continue
                        ;;
                esac
                if printf '%s' "$dll" | grep -Eq "$FORBIDDEN_CRT_PATTERN"; then
                    bad "$binary imports the CRT redistributable $dll"
                    note "the release contract is a static CRT; no vcredist is shipped"
                    continue
                fi
                case " $(printf '%s' "$KNOWN_SYSTEM_IMPORTS" | tr '\n' ' ') " in
                    *" $dll "*) : ;;
                    *) warn "$binary imports $dll, which is outside the known"
                       note "Wine-provided set; confirm your Wine build supplies it" ;;
                esac
            done
        else
            warn "$binary import table could not be parsed"
        fi
    done

    section "4. DirectDraw wrapper exports and SDL3 runtime"

    wrapper="$PKG_DIR/ddraw.dll"
    if [ -f "$wrapper" ] && lfanew=$(pe_lfanew "$wrapper"); then
        if exports=$(pe_exports "$wrapper" "$lfanew"); then
            for required in $REQUIRED_WRAPPER_EXPORTS; do
                line=$(printf '%s\n' "$exports" | grep -E "^[0-9]+ $required\$" || true)
                if [ -n "$line" ]; then
                    ok "ddraw.dll exports $required (ordinal ${line%% *})"
                else
                    bad "ddraw.dll does not export $required"
                    note "Wine's builtin DirectDraw is NOT a substitute: netplay"
                    note "refuses to run without this pacing ABI."
                fi
            done
            if [ "$VERBOSE" = "1" ]; then
                printf '%s\n' "$exports" | while read -r ordinal name; do
                    vnote "ddraw.dll @$ordinal $name"
                done
            fi
        else
            bad "ddraw.dll has no readable export directory"
        fi
    else
        bad "ddraw.dll is missing or not a PE image"
    fi

    sdl="$PKG_DIR/SDL3.dll"
    if [ -f "$sdl" ]; then
        if sdl_sha=$(sha256_of "$sdl"); then
            if [ "$(lower "$sdl_sha")" = "$PINNED_SDL3_SHA256" ]; then
                ok "SDL3.dll is the pinned controller runtime"
            else
                bad "SDL3.dll is not the pinned artifact (got $sdl_sha)"
                note "expected $PINNED_SDL3_SHA256"
            fi
        else
            skip "no hashing tool; SDL3.dll identity unverified"
        fi
    else
        bad "SDL3.dll is missing; controller support degrades to DirectInput8"
    fi

    section "5. caster/DLL build contract"

    caster="$PKG_DIR/qoh99caster.exe"
    netplay_dll="$PKG_DIR/qoh99_netplay.dll"
    if [ -f "$caster" ] && [ -f "$netplay_dll" ]; then
        caster_contract=$(binary_marker "$caster" "$CONTRACT_PREFIX")
        dll_contract=$(binary_marker "$netplay_dll" "$CONTRACT_PREFIX")
        if [ -z "$caster_contract" ] || [ -z "$dll_contract" ]; then
            bad "the ${CONTRACT_PREFIX}* marker is absent from the caster or DLL"
        elif [ "$caster_contract" = "$dll_contract" ]; then
            ok "caster and netplay DLL carry the same build contract"
            vnote "$caster_contract"
        else
            bad "qoh99caster.exe and qoh99_netplay.dll are from different builds"
            note "caster: $caster_contract"
            note "dll   : $dll_contract"
        fi
    else
        skip "caster or netplay DLL missing; contract not compared"
    fi

    for stray in ddraw.dll qoh99_netplay.dll qoh99caster.exe SDL3.dll; do
        if [ -f "$GAME_DIR/$stray" ] && [ -f "$PKG_DIR/$stray" ] \
           && [ "$GAME_DIR" != "$PKG_DIR" ]; then
            if ! cmp -s "$GAME_DIR/$stray" "$PKG_DIR/$stray"; then
                bad "$GAME_DIR/$stray differs from the packaged $stray"
                note "extract the whole archive beside the game; never mix builds"
            fi
        fi
    done

    section "6. executable, game and state paths"

    path_report() { # path label legacy(0|1)
        local path="$1" label="$2" legacy="$3" length
        length=${#path}
        if ! is_ascii "$path"; then
            warn "$label contains non-ASCII characters: $path"
            note "the game's own legacy loader is ANSI; a prefix code page"
            note "mismatch can make its assets unreachable"
        fi
        case "$path" in
            *" "*) vnote "$label contains a space (supported, but quote it)" ;;
        esac
        if [ "$legacy" = "1" ]; then
            if [ "$length" -gt "$LEGACY_ASSET_PATH_MAX" ]; then
                bad "$label is $length characters; QOH's legacy asset loader"
                note "truncates any absolute asset path above"
                note "$LEGACY_ASSET_PATH_MAX bytes - move the install shallower"
            elif [ "$length" -gt "$LEGACY_ASSET_PATH_WARN" ]; then
                warn "$label is $length characters; every asset path below it"
                note "must still fit in $LEGACY_ASSET_PATH_MAX bytes"
            else
                ok "$label length $length leaves room for the legacy loader"
            fi
        elif [ "$length" -gt 200 ]; then
            warn "$label is $length characters; keep it well short of MAX_PATH"
        else
            ok "$label length $length is safe"
        fi
    }

    path_report "$GAME_DIR" "game directory" 1
    path_report "$PKG_DIR" "package directory" 0

    if [ -d "$STATE_DIR" ]; then
        probe_base="$STATE_DIR/.qoh99_preflight_$$"
        if : > "$probe_base.tmp" 2>/dev/null; then
            if mv "$probe_base.tmp" "$probe_base.renamed" 2>/dev/null; then
                if rm -f "$probe_base.renamed" 2>/dev/null; then
                    ok "state directory is writable (create/rename/delete)"
                else
                    bad "state directory: created and renamed, but could not delete"
                    rm -f "$probe_base.renamed" 2>/dev/null || true
                fi
            else
                bad "state directory: created, but atomic rename failed"
                rm -f "$probe_base.tmp" 2>/dev/null || true
            fi
        else
            bad "state directory is not writable: $STATE_DIR"
            note "logs/session_*, qoh99caster.ini and QOHcnf.key all need it;"
            note "set QOH99_NETPLAY_STATE_DIR to a writable directory"
        fi

        case_probe="$STATE_DIR/.qoh99Case_$$"
        if : > "$case_probe" 2>/dev/null; then
            if [ -e "$(printf '%s' "$case_probe" | tr 'C' 'c')" ]; then
                note "filesystem is case-insensitive (Windows-like)"
            else
                vnote "filesystem is case-sensitive (Linux default)"
            fi
            rm -f "$case_probe" 2>/dev/null || true
        fi
    else
        bad "state directory does not exist: $STATE_DIR"
    fi

    mount_of() { # path -> "mountpoint fstype options"
        local path="$1" best="" line target type options
        [ -r /proc/mounts ] || return 1
        while read -r _ target type options _; do
            case "$path/" in
                "$target"/*|"$target")
                    if [ "${#target}" -ge "${#best}" ]; then
                        best="$target"
                        MOUNT_TARGET="$target"
                        MOUNT_TYPE="$type"
                        MOUNT_OPTIONS="$options"
                    fi
                    ;;
            esac
        done < /proc/mounts
        [ -n "$best" ] || return 1
        return 0
    }

    MOUNT_TARGET=""; MOUNT_TYPE=""; MOUNT_OPTIONS=""
    if mount_of "$GAME_DIR"; then
        ok "game filesystem: $MOUNT_TYPE on $MOUNT_TARGET"
        vnote "mount options: $MOUNT_OPTIONS"
        case ",$MOUNT_OPTIONS," in
            *,ro,*) bad "the game filesystem is mounted read-only" ;;
        esac
        case ",$MOUNT_OPTIONS," in
            *,noexec,*) bad "the game filesystem is mounted noexec; Wine cannot map it" ;;
        esac
        case "$MOUNT_TYPE" in
            ntfs|ntfs3|fuseblk|lowntfs-3g)
                warn "**********************************************************"
                warn "* NTFS WARNING: the game lives on an NTFS mount. *"
                warn "**********************************************************"
                note "A Windows/Linux shared Steam library is operationally"
                note "fragile: prefix ownership, symlinks, permissions, filename"
                note "semantics and case behaviour can all fail in ways that have"
                note "nothing to do with the netplay code. The supported baseline"
                note "is a native Linux filesystem (ext4 or btrfs). Validate"
                note "there FIRST before reporting any problem."
                note "See https://github.com/ValveSoftware/Proton/wiki/Using-a-NTFS-disk-with-Linux-and-Windows"
                ;;
            exfat|vfat|msdos)
                warn "the game is on $MOUNT_TYPE: no permissions and no case"
                note "sensitivity; use ext4 or btrfs for the supported baseline"
                ;;
            9p|drvfs|virtiofs|cifs|smb3|nfs|nfs4)
                warn "the game is on $MOUNT_TYPE (network/translation layer);"
                note "expect latency and locking behaviour Wine has not been"
                note "qualified against"
                ;;
            ext4|btrfs|xfs|f2fs|zfs|overlay|tmpfs)
                vnote "supported-class filesystem"
                ;;
        esac
    else
        skip "no /proc/mounts; filesystem and mount options unknown"
    fi

    section "7. compatibility layer"

    if [ -z "$WINE_BIN" ]; then
        if have wine; then WINE_BIN=wine; fi
    fi

    WINE_USABLE=0
    if [ -n "$WINE_BIN" ] && have "$WINE_BIN"; then
        if wine_version=$("$WINE_BIN" --version 2>/dev/null); then
            ok "wine binary: $WINE_BIN ($wine_version)"
            WINE_USABLE=1
        else
            bad "'$WINE_BIN' did not answer --version"
        fi
    else
        bad "no usable Wine binary (looked at --wine, \$WINE, and PATH)"
        note "install a maintained Wine, or run through Steam/Proton and use"
        note "the Wine and Proton section of README.txt instead of these scripts"
    fi

    if [ -z "$WINE_PREFIX" ]; then WINE_PREFIX="${HOME:-~}/.wine"; fi
    if [ -d "$WINE_PREFIX" ]; then
        ok "WINEPREFIX: $WINE_PREFIX (existing; this script never modifies it)"
    else
        warn "WINEPREFIX $WINE_PREFIX does not exist yet"
        note "the first launch creates it; that first run is slow, not stuck"
    fi
    if [ -n "${WINEARCH:-}" ]; then
        note "WINEARCH=$WINEARCH is set in your environment (left untouched)"
    fi

    proton_context=0
    for variable in STEAM_COMPAT_DATA_PATH STEAM_COMPAT_CLIENT_INSTALL_PATH \
                    SteamAppId SteamGameId PROTON_LOG PROTON_USE_WINED3D; do
        value=$(eval "printf '%s' \"\${$variable:-}\"")
        if [ -n "$value" ]; then
            proton_context=1
            note "$variable=$value"
        fi
    done
    if [ "$proton_context" = "1" ]; then
        ok "Proton/Steam title context detected"
    else
        vnote "no Proton/Steam compatibility variables in this environment"
    fi

    if [ "$WINE_USABLE" = "1" ] && [ -f "$PKG_DIR/qoh99caster.exe" ]; then
        if caster_help=$(WINEDEBUG="${WINEDEBUG:--all}" \
                         "$WINE_BIN" "$PKG_DIR/qoh99caster.exe" help 2>/dev/null); then
            if printf '%s' "$caster_help" | grep -q "qoh99caster"; then
                ok "the prefix executed the packaged 32-bit caster"
            else
                warn "the caster ran but printed unexpected output"
            fi
        else
            bad "Wine could not execute the packaged PE32 caster"
            note "a wow64/32-bit-capable prefix and 32-bit graphics/driver"
            note "packages are required"
        fi
    else
        skip "PE32 execution not attempted (no wine binary or no caster)"
    fi

    section "8. native DirectDraw override policy"

    current_overrides="${WINEDLLOVERRIDES:-}"
    merged_overrides=$(merge_ddraw_override "$current_overrides")
    if [ -n "$current_overrides" ]; then
        note "current WINEDLLOVERRIDES: $current_overrides"
    fi
    note "effective for the game  : $merged_overrides"
    case "$merged_overrides" in
        *"ddraw=n,b"*) ok "the packaged native ddraw.dll will be selected" ;;
        *) bad "the merge did not produce a native ddraw rule" ;;
    esac
    kept=${merged_overrides%;ddraw=n,b}
    if [ "$kept" = "$merged_overrides" ]; then kept=""; fi
    if [ -n "$kept" ]; then
        ok "unrelated overrides preserved: $kept"
    fi
    if printf '%s' "$current_overrides" | grep -Eqi '(^|;|,)ddraw(\.dll)?(,|=)'; then
        note "an existing ddraw rule was replaced, not appended to; this is the"
        note "same rule src/platform/CompatEnv.cpp applies to the child process"
    fi

    section "9. display and session"

    display_routes=""
    [ -n "${DISPLAY:-}" ] && display_routes="$display_routes X11($DISPLAY)"
    [ -n "${WAYLAND_DISPLAY:-}" ] && display_routes="$display_routes Wayland($WAYLAND_DISPLAY)"
    [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ] && display_routes="$display_routes Gamescope($GAMESCOPE_WAYLAND_DISPLAY)"
    if [ -n "${XDG_SESSION_TYPE:-}" ]; then note "XDG_SESSION_TYPE=$XDG_SESSION_TYPE"; fi
    if [ -n "${XDG_RUNTIME_DIR:-}" ]; then vnote "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"; fi
    if [ -n "$display_routes" ]; then
        ok "display route(s):$display_routes"
        if [ -z "${DISPLAY:-}" ] && [ -n "${WAYLAND_DISPLAY:-}" ]; then
            warn "no DISPLAY: Wine needs XWayland unless this build has the"
            note "native Wayland driver; that route is not in the tested tier"
        fi
    else
        bad "no DISPLAY, WAYLAND_DISPLAY or Gamescope socket: nothing can render"
    fi

    section "10. process, injection, IPC and socket-duplication fixture"

    if [ "$WINE_USABLE" = "1" ] && [ -f "$PKG_DIR/qoh99caster.exe" ]; then
        set +e
        fixture_output=$(WINEDEBUG="${WINEDEBUG:--all}" \
            "$WINE_BIN" "$PKG_DIR/qoh99caster.exe" --wine-fixture 2>&1)
        fixture_status=$?
        set -e
        if [ "$fixture_status" -eq 0 ]; then
            ok "compatibility fixture passed"
            printf '%s\n' "$fixture_output" | sed 's/^/         /'
        elif ! printf '%s' "$fixture_output" | grep -q '^platform' \
             || printf '%s' "$fixture_output" \
                | grep -Eqi 'unsupported|unknown (option|verb)|unrecognized'; then
            skip "this caster build has no --wine-fixture verb"
            note "process creation, remote injection, named pipes, Local\\ events"
            note "and WSADuplicateSocketW therefore remain UNPROVEN on this host"
        else
            bad "compatibility fixture failed (exit $fixture_status)"
            printf '%s\n' "$fixture_output" | sed 's/^/         /'
        fi
    else
        skip "fixture not attempted (no wine binary or no caster)"
    fi

    section "11. local socket binds"

    GAME_PORT="${QOH99_PREFLIGHT_PORT:-$DEFAULT_GAME_PORT}"
    SPECTATOR_PORT=$(( GAME_PORT + 1 ))

    port_listener_present() { # proto port -> 0 when something already holds it
        local proto="$1" port="$2"
        if have ss; then
            ss -H -ln --"$proto" 2>/dev/null | grep -Eq "[:.]$port\$|[:.]$port[[:space:]]"
        elif have netstat; then
            netstat -ln 2>/dev/null | grep -Eq "^$proto.*[:.]$port[[:space:]]"
        else
            return 2
        fi
    }

    bind_probe() { # proto port -> 0 bound, 1 refused, 2 no tool
        local proto="$1" port="$2" pid status
        if have socat; then
            if [ "$proto" = tcp ]; then
                socat -u TCP4-LISTEN:"$port",bind=127.0.0.1,reuseaddr /dev/null                     2>/dev/null &
            else
                socat -u UDP4-RECVFROM:"$port",bind=127.0.0.1,reuseaddr /dev/null                     2>/dev/null &
            fi
            pid=$!
            sleep 0.3
            if kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                return 0
            fi
            wait "$pid" 2>/dev/null || true
            return 1
        fi
        if have nc; then
            if [ "$proto" = tcp ]; then
                nc -l 127.0.0.1 "$port" >/dev/null 2>&1 &
            else
                nc -u -l 127.0.0.1 "$port" >/dev/null 2>&1 &
            fi
            pid=$!
            sleep 0.3
            if kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                return 0
            fi
            wait "$pid" 2>/dev/null || true
            return 1
        fi
        return 2
    }

    check_port() { # proto port label
        local proto="$1" port="$2" label="$3" status
        set +e
        port_listener_present "$proto" "$port"
        status=$?
        set -e
        if [ "$status" -eq 0 ]; then
            bad "$label $proto/$port is already in use"
            return
        fi
        set +e
        bind_probe "$proto" "$port"
        status=$?
        set -e
        case "$status" in
            0) ok "$label $proto/$port binds" ;;
            1) bad "$label $proto/$port could not be bound" ;;
            *) warn "$label $proto/$port: no free listener yet, but no socat/nc"
               note "is installed, so a real bind was not proven" ;;
        esac
    }

    check_port tcp "$GAME_PORT" "host control"
    check_port udp "$GAME_PORT" "player data"
    check_port udp "$SPECTATOR_PORT" "spectator data"
    note "both the TCP and the UDP port must be allowed through the host"
    note "firewall, plus the adjacent spectator UDP port."

    section "12. relay service"

    if [ -n "$RELAY_TARGET" ]; then
        relay_host=${RELAY_TARGET%:*}
        relay_port=${RELAY_TARGET##*:}
        if [ "$relay_host" = "$RELAY_TARGET" ]; then relay_port=14764; fi
        if [ -z "$relay_host" ] || [ -z "$relay_port" ]; then
            bad "--relay must be host[:port]; got '$RELAY_TARGET'"
        else
            if printf '%s' "$relay_host" \
               | grep -Eq '^[0-9]{1,3}(\.[0-9]{1,3}){3}$'; then
                ok "relay address $relay_host:$relay_port is numeric IPv4"
            else
                warn "the netplay core accepts numeric IPv4 only;"
                note "'$relay_host' would have to be resolved by hand"
                if have getent; then
                    resolved=$(getent hosts "$relay_host" | head -n1 | cut -d' ' -f1)
                    if [ -n "$resolved" ]; then
                        note "resolves to $resolved"
                        relay_host="$resolved"
                    else
                        bad "'$relay_host' does not resolve"
                    fi
                fi
            fi
            set +e
            if have timeout; then
                timeout 4 bash -c \
                    "exec 3<>/dev/tcp/$relay_host/$relay_port" 2>/dev/null
            else
                bash -c "exec 3<>/dev/tcp/$relay_host/$relay_port" 2>/dev/null
            fi
            relay_status=$?
            set -e
            if [ "$relay_status" -eq 0 ]; then
                ok "relay TCP $relay_host:$relay_port accepted a connection"
                note "the same numeric port must also pass UDP; that cannot be"
                note "proven from here"
            else
                bad "relay TCP $relay_host:$relay_port refused or timed out"
                note "connection refused usually means no relay is running at"
                note "that address. Start one with: qoh99caster relay --bind"
                note "0.0.0.0 --port $relay_port"
            fi
        fi
    else
        skip "no --relay <host:port> given; relay reachability unchecked"
    fi

    section "13. summary"

    printf '  failures: %s   warnings: %s   skipped: %s\n' \
           "$FAILURES" "$WARNINGS" "$SKIPS"

    printf '\n  If anything above failed, or the game misbehaves later, collect:\n'
    printf '    %s\n' "$GAME_DIR/logs/session_*/"
    printf '    %s\n' "$STATE_DIR/qoh99caster.ini"
    printf '    %s\n' "$STATE_DIR/qoh99caster-controls.json"
    printf '    %s\n' "$GAME_DIR/ddraw_wrapper.ini"
    printf '    %s\n' "$GAME_DIR/qoh99_netplay.log"
    printf '    %s\n' "$WINE_PREFIX/ (name and version only, never the whole prefix)"
    if [ -n "${STEAM_COMPAT_DATA_PATH:-}" ]; then
        printf '    %s\n' "$STEAM_COMPAT_DATA_PATH/pfx (Proton prefix path)"
        printf '    %s\n' "~/.steam/steam/logs/ and steam-<appid>.log (PROTON_LOG=1)"
    fi
    printf '\n  One command that captures the usual set:\n'
    printf '    tar czf qoh99-support-$(date +%%Y%%m%%d-%%H%%M%%S).tar.gz \\\n'
    printf '        -C "%s" logs ddraw_wrapper.ini qoh99_netplay.log 2>/dev/null\n' \
           "$GAME_DIR"
    printf '  Also record: wine --version, your distribution and kernel, GPU and\n'
    printf '  driver, X11/Wayland/Gamescope, Steam Overlay and Steam Input state,\n'
    printf '  how the controller is connected, the filesystem, and whether the\n'
    printf '  prefix was fresh.\n'

    printf '\n  Preflight success is NECESSARY but NEVER SUFFICIENT. Passing every\n'
    printf '  check above does not prove Direct3D presentation, controllers,\n'
    printf '  audio, gameplay speed, rollback, spectators or rematch. Wine/Proton\n'
    printf '  support for this package is NOT VERIFIED on real Linux hardware.\n'
    printf '  See the Wine and Proton section in README.txt.\n'

    if [ "$FAILURES" -gt 0 ]; then
        printf '\nPREFLIGHT FAILED (%s hard failure(s)).\n' "$FAILURES"
        return 1
    fi
    printf '\nPreflight passed with %s warning(s) and %s skipped check(s).\n' \
           "$WARNINGS" "$SKIPS"
    return 0

}

marker_key() {
    manifest_digest='none'
    if [ -f "$PKG_DIR/manifest.sha256" ]; then
        manifest_digest=$(sha256_of "$PKG_DIR/manifest.sha256" 2>/dev/null \
            || echo unreadable)
    fi
    game_identity='none'
    if [ -f "$GAME_DIR/qoh99.exe" ]; then
        game_identity="$(size_of "$GAME_DIR/qoh99.exe" 2>/dev/null || echo 0)"
        game_identity="$game_identity/$(date -r "$GAME_DIR/qoh99.exe" \
            '+%s' 2>/dev/null || echo 0)"
    fi
    wine_identity='none'
    if [ -n "$WINE_BIN" ] && have "$WINE_BIN"; then
        wine_identity=$("$WINE_BIN" --version 2>/dev/null || echo unknown)
    fi
    printf 'v%s|%s|%s|%s|%s\n' "$SCRIPT_VERSION" "$manifest_digest" \
           "$game_identity" "$wine_identity" "$GAME_DIR"
}

marker_is_current() {
    [ -f "$MARKER_PATH" ] || return 1
    stored=$(head -n 1 "$MARKER_PATH" 2>/dev/null || true)
    [ -n "$stored" ] || return 1
    [ "$stored" = "$(marker_key)" ]
}

marker_write() {
    {
        marker_key
        printf '# QOH99 netplay: the checks in launch_wine.sh passed for the\n'
        printf '# exact package, game build, wine version and directory named\n'
        printf '# on the first line. Delete this file, or pass --recheck, to\n'
        printf '# run them again. Changing any of those re-runs them anyway.\n'
        printf '# written: %s\n' \
               "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)"
    } > "$MARKER_PATH" 2>/dev/null || true
}

run_checks_logged() {
    mkdir -p "$GAME_DIR/logs" 2>/dev/null || true
    if : > "$CHECK_LOG" 2>/dev/null; then
        run_checks 2>&1 | tee "$CHECK_LOG"
    else
        CHECK_LOG=""
        run_checks
    fi
}

clear_console() {
    [ "$NO_CLEAR" = "1" ] && return 0
    [ -t 1 ] || return 0
    if have clear; then
        clear
    else
        printf '\033[2J\033[H'
    fi
}

checks_ran=0
checks_status=0

if [ "$CHECK_ONLY" = "1" ]; then
    set +e; run_checks_logged; checks_status=$?; set -e
    if [ "$checks_status" -eq 0 ]; then marker_write; fi
    exit "$checks_status"
fi

if [ "$SKIP_PREFLIGHT" = "1" ]; then
    printf 'launch_wine.sh: checks skipped by request.\n'
elif [ "$RECHECK" != "1" ] && marker_is_current; then
    printf 'launch_wine.sh: checks already passed for this package, game\n'
    printf '                build, wine version and directory - skipping.\n'
    printf '                Re-run them with --recheck, or delete %s\n' \
           "$MARKER_PATH"
else
    set +e; run_checks_logged; checks_status=$?; set -e
    checks_ran=1
    if [ "$checks_status" -eq 0 ]; then
        marker_write
    elif [ "$STRICT" = "1" ]; then
        printf '\nlaunch_wine.sh: ABORTED - checks failed and --strict was\n' >&2
        printf '                given.\n' >&2
        exit "$checks_status"
    else
        printf '\nlaunch_wine.sh: checks reported failures. Launching anyway:\n'
        printf '                the caster verifies the package itself and will\n'
        printf '                refuse with a precise reason if it is really\n'
        printf '                unusable. Use --strict to stop here instead.\n'
    fi
fi

if [ -z "$WINE_BIN" ]; then
    have wine || die "no wine binary found; pass --wine or set \$WINE"
    WINE_BIN=wine
fi
have "$WINE_BIN" || die "wine binary is not executable: $WINE_BIN"

if [ -n "$WINE_PREFIX" ]; then
    WINEPREFIX=$(abspath "$WINE_PREFIX")
    export WINEPREFIX
fi

WINEDLLOVERRIDES=$(merge_ddraw_override "${WINEDLLOVERRIDES:-}")
export WINEDLLOVERRIDES

clear_console

printf '\nlaunch_wine.sh\n'
printf '  wine            : %s (%s)\n' "$WINE_BIN" \
       "$("$WINE_BIN" --version 2>/dev/null || echo 'version unknown')"
printf '  WINEPREFIX      : %s\n' "${WINEPREFIX:-${HOME:-~}/.wine (default)}"
printf '  WINEDLLOVERRIDES: %s\n' "$WINEDLLOVERRIDES"
printf '  package         : %s\n' "$PKG_DIR"
printf '  working dir     : %s\n' "$GAME_DIR"
printf '  launching       : qoh99caster.exe (the caster starts the game)\n'
if [ "$WINE_STDERR_TO_TERMINAL" = "1" ]; then
    printf '  wine messages   : this terminal (--wine-stderr)\n'
else
    printf '  wine messages   : %s\n' "$WINE_LOG"
fi
if [ -n "$CHECK_LOG" ] && [ "$checks_ran" = "1" ]; then
    printf '  checks          : %s\n' "$CHECK_LOG"
fi

cd "$GAME_DIR"
mkdir -p "$GAME_DIR/logs" 2>/dev/null || true
if ! : >> "$WINE_LOG" 2>/dev/null; then
    WINE_LOG="/dev/null"
fi

CONSOLE_LAUNCHER="$WINE_BIN"
CONSOLE_PREFIX=""
if [ "$CONSOLE_MODE" != "terminal" ]; then
    if have wineconsole; then
        if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
            CONSOLE_PREFIX="wineconsole"
        elif [ "$CONSOLE_MODE" = "window" ]; then
            die "--window needs a display; none of DISPLAY or WAYLAND_DISPLAY is set"
        fi
    elif [ "$CONSOLE_MODE" = "window" ]; then
        die "--window needs wineconsole, which is not on PATH"
    fi
fi

if [ -n "$CONSOLE_PREFIX" ]; then
    QOH99_CONSOLE_VT=0
    printf '  console         : own window (wineconsole), cell rendering
'
else
    QOH99_CONSOLE_VT=1
    printf '  console         : this terminal, VT rendering
'
fi
export QOH99_CONSOLE_VT

set +e
if [ -n "$CONSOLE_PREFIX" ]; then
    "$CONSOLE_PREFIX" "$CASTER" "$@" 2>>"$WINE_LOG"
    status=$?
elif [ "$WINE_STDERR_TO_TERMINAL" = "1" ]; then
    "$CONSOLE_LAUNCHER" "$CASTER" "$@"
    status=$?
else
    "$CONSOLE_LAUNCHER" "$CASTER" "$@" 2>>"$WINE_LOG"
    status=$?
fi
set -e

if [ "$status" -ne 0 ]; then
    printf '\nqoh99caster.exe exited with status %s.\n' "$status" >&2
    printf 'Collect a support bundle with exactly this command:\n\n' >&2
    printf '  tar czf qoh99-support-$(date +%%Y%%m%%d-%%H%%M%%S).tar.gz \\\n' >&2
    printf '      -C "%s" logs ddraw_wrapper.ini qoh99_netplay.log\n\n' \
           "$GAME_DIR" >&2
    printf 'Include with it: `%s --version`, your distribution and kernel,\n' \
           "$WINE_BIN" >&2
    printf 'GPU and driver, X11/Wayland/Gamescope, Steam Overlay and Steam\n' >&2
    printf 'Input state, how the controller is connected, the filesystem\n' >&2
    printf 'holding the game, and whether the prefix was fresh.\n' >&2
    printf 'Under Proton, add PROTON_LOG=1 and attach steam-<appid>.log.\n' >&2
fi

exit "$status"

#!/bin/sh
# naslab_agent.sh -- bring up qoh99_e2e_agent.exe under Wine on the lab box.
#
# Deployed to $LAB and started over SSH by rolehost.SshRoleHost.  It reuses the
# Wine environment naslab.sh proved out, and fixes the traps that cost real
# time to find:
#
#  * Xvfb REJECTS depth 32 ("Couldn't add screen 0").  naslab.sh:27 still
#    starts it at x32, so this script NEVER calls naslab.sh start_x and always
#    uses x24.  A silently dead Xvfb looks exactly like a graphics
#    incompatibility, so it is verified up rather than assumed.
#  * An install clone carries the SOURCE machine's ddraw_wrapper.ini window
#    size.  Left alone, llvmpipe rasterises megapixels and the run crawls.
#    ClientWidth/ClientHeight are reset to 0 in every clone before a run.
#  * The install uses UPPERCASE names (SYSTEM/QOHCNF.KEY, CONFIG.EXE).  Linux
#    shell paths are case-sensitive; Wine copes, so nothing here may assume a
#    lowercase spelling.
#  * QOH's legacy OpenFile stores the resolved name in OFSTRUCT.szPathName[128].
#    Under Wine an install below $LAB is reached as
#    Z:\srv\dev-disk-by-uuid-...\appdata\qoh99lab\..., which burns ~70 of the
#    127 usable characters before the run root even starts.  A short DOS drive
#    (Q: -> $LAB/e2e) is created so clones are Q:\<run>\h\g\... instead.
#
# Verbs:
#   stdio <lease_ms>        THE VERB THE HARNESS USES. Prepare the environment,
#                           then exec the agent speaking the protocol on stdin/
#                           stdout so it needs no listening port and no SSH
#                           port-forward. The lab's sshd answers a forward
#                           request with "administratively prohibited: open
#                           failed", so `ssh -L` is not available here -- and
#                           stdio is better anyway: no port, no forward process,
#                           no connect-before-ready race.
#                           EVERY diagnostic must go to stderr; anything printed
#                           on stdout corrupts the wire.
#   run <port> <lease_ms>   same, but on loopback TCP (local protocol testing)
#   drive-root              create the DOS drive if absent and print the POSIX
#                           directory it resolves to, on STDOUT, nothing else.
#                           The controller stages build artifacts over scp,
#                           which speaks POSIX paths, but then asks the agent
#                           to open them as `Q:\bin\...`, which Wine resolves
#                           through this symlink.  Those two halves MUST name
#                           one directory; when they did not, staging wrote
#                           $LAB/bin while the agent opened $LAB/e2e/bin and
#                           reported GetLastError=3.  Asking the prefix is the
#                           only answer that cannot drift.
#   clean                   remove stale run roots under $LAB/e2e
#   doctor                  print what is and is not ready, and exit non-zero
#                           if the environment cannot host a run

set -u

LAB="${LAB:-$(cd "$(dirname "$0")" && pwd)}"
PREFIX="${WINEPREFIX:-$LAB/prefix}"
DISPLAY_NUMBER="${QOH_DISPLAY:-:99}"
DRIVE_LETTER="${QOH_DRIVE:-q}"
E2E_ROOT="$LAB/e2e"
# BOTH halves of the staging path are derived from E2E_ROOT, because that is
# what `<drive>:` resolves to.  The controller scp's build artifacts to
# `$(drive-root)/bin` and then asks the agent to open `Q:\bin\...`; when this
# line said "$LAB/bin" instead, the two named different directories and the
# only symptom was GetLastError=3 on a file that plainly existed.
BIN_DIR="$E2E_ROOT/bin"
AGENT_EXE="$BIN_DIR/qoh99_e2e_agent.exe"
LOCK_FILE="$LAB/.qoh99-e2e.lock"

export WINEARCH=win32
export WINEPREFIX="$PREFIX"
export WINEDEBUG=-all
export DISPLAY="$DISPLAY_NUMBER"
export LIBGL_ALWAYS_SOFTWARE=1
export GALLIUM_DRIVER=llvmpipe
export WINEDLLOVERRIDES="ddraw=n,b;dinput=n,b"

log() { printf '[naslab-agent] %s\n' "$*" >&2; }

display_is_up() {
    # xdpyinfo is the direct answer but is NOT installed on the lab box (its
    # absence was found by `doctor`, not by a failed run).  The X11 socket for
    # the display is created by the server once it is actually listening, so
    # it is an equivalent liveness signal that needs no extra package.
    if command -v xdpyinfo >/dev/null 2>&1; then
        xdpyinfo -display "$DISPLAY_NUMBER" >/dev/null 2>&1
        return $?
    fi
    [ -S "/tmp/.X11-unix/X${DISPLAY_NUMBER#:}" ]
}

start_display() {
    # Depth 24. NEVER 32: Xvfb rejects x32 with "Couldn't add screen 0" and
    # then dies, which is indistinguishable from a graphics incompatibility
    # further down the stack.  That is why the readiness check below is a
    # check and not an assumption.
    if display_is_up; then
        log "display $DISPLAY_NUMBER already up"
        return 0
    fi
    log "starting Xvfb on $DISPLAY_NUMBER at depth 24"
    Xvfb "$DISPLAY_NUMBER" -screen 0 1024x768x24 \
        +extension GLX +extension RENDER -noreset \
        >"$LAB/xvfb.log" 2>&1 &
    xvfb_pid=$!
    waited=0
    while [ "$waited" -lt 100 ]; do
        if display_is_up; then
            log "display $DISPLAY_NUMBER is up"
            return 0
        fi
        if ! kill -0 "$xvfb_pid" 2>/dev/null; then
            log "FATAL: Xvfb exited immediately; last lines of $LAB/xvfb.log:"
            tail -5 "$LAB/xvfb.log" >&2 2>/dev/null
            return 1
        fi
        waited=$((waited + 1))
        sleep 0.1
    done
    log "FATAL: Xvfb did not come up on $DISPLAY_NUMBER; see $LAB/xvfb.log"
    return 1
}

prepare_drive() {
    # Short DOS drive for the legacy 127-character asset-path ceiling.
    mkdir -p "$E2E_ROOT" "$PREFIX/dosdevices" || return 1
    link="$PREFIX/dosdevices/${DRIVE_LETTER}:"
    if [ -L "$link" ]; then
        current=$(readlink "$link")
        if [ "$current" != "$E2E_ROOT" ]; then
            rm -f "$link"
        fi
    fi
    if [ ! -e "$link" ]; then
        ln -s "$E2E_ROOT" "$link" || return 1
    fi
    log "drive ${DRIVE_LETTER}: -> $E2E_ROOT"
}

reset_clone_window_sizes() {
    # An install clone inherits the SOURCE machine's window size from
    # ddraw_wrapper.ini.  On llvmpipe that turns into megapixels of software
    # rasterisation per frame; 0/0 restores the wrapper's own default.
    find "$E2E_ROOT" -name 'ddraw_wrapper.ini' 2>/dev/null | while read -r ini; do
        sed -i -E 's/^([[:space:]]*Client(Width|Height)[[:space:]]*=).*/\1 0/I' \
            "$ini" 2>/dev/null || true
    done
}

doctor() {
    status=0
    for tool in wine Xvfb; do
        if command -v "$tool" >/dev/null 2>&1; then
            log "ok: $tool"
        else
            log "MISSING: $tool"
            status=1
        fi
    done
    if command -v xdpyinfo >/dev/null 2>&1; then
        log "ok: xdpyinfo (preferred display probe)"
    else
        log "note: xdpyinfo absent; falling back to the X11 socket probe"
    fi
    [ -d "$PREFIX" ] && log "ok: prefix $PREFIX" || {
        log "MISSING: wine prefix $PREFIX"; status=1; }
    [ -f "$AGENT_EXE" ] && log "ok: agent $AGENT_EXE" || {
        log "MISSING: staged agent $AGENT_EXE"; status=1; }
    if [ -f "$LOCK_FILE" ]; then
        log "note: lock file present ($LOCK_FILE) -- a run may be in flight"
    fi
    return "$status"
}

case "${1:-stdio}" in
    stdio)
        LEASE_MS="${2:-30000}"
        if [ ! -f "$AGENT_EXE" ]; then
            log "FATAL: $AGENT_EXE has not been staged"
            exit 1
        fi
        start_display || exit 1
        prepare_drive || exit 1
        reset_clone_window_sizes
        log "exec agent (stdio) lease=${LEASE_MS}ms"
        # stdout is THE WIRE. Wine's own chatter is already suppressed by
        # WINEDEBUG=-all, and every log() above writes to stderr, which the
        # caller redirects to agent.boot.log.
        exec wine "$AGENT_EXE" \
            --stdio \
            --lease-ms "$LEASE_MS" \
            --lock "${DRIVE_LETTER}:\\.qoh99-e2e.lock"
        ;;
    run)
        PORT="${2:-47311}"
        LEASE_MS="${3:-30000}"
        if [ ! -f "$AGENT_EXE" ]; then
            log "FATAL: $AGENT_EXE has not been staged"
            exit 1
        fi
        start_display || exit 1
        prepare_drive || exit 1
        reset_clone_window_sizes
        log "exec agent port=$PORT lease=${LEASE_MS}ms"
        # Foreground: the caller backgrounds this with nohup, and the LEASE --
        # not the SSH channel -- is what bounds the agent's life.
        exec wine "$AGENT_EXE" \
            --port "$PORT" \
            --lease-ms "$LEASE_MS" \
            --lock "${DRIVE_LETTER}:\\.qoh99-e2e.lock"
        ;;
    drive-root)
        # STDOUT is the answer and carries nothing else; log() writes stderr.
        prepare_drive || exit 1
        link="$PREFIX/dosdevices/${DRIVE_LETTER}:"
        resolved=$(readlink -f "$link" 2>/dev/null)
        if [ -z "$resolved" ] || [ ! -d "$resolved" ]; then
            log "FATAL: ${DRIVE_LETTER}: does not resolve to a directory"
            exit 1
        fi
        printf '%s\n' "$resolved"
        ;;
    clean)
        # ALLOWLIST, not a name pattern.  Run roots are named by the
        # controller (`<tags>_<pid>_<nonce>`, e.g. pidt_59b0_5f0723), so the
        # old `-name 'live_*'` filter matched nothing and this verb silently
        # cleaned zero bytes while every run left ~440 MB behind.  Name what
        # must SURVIVE instead: the staged build (bin) and the staged source
        # install (game).  Everything else under $E2E_ROOT is disposable.
        for entry in "$E2E_ROOT"/*; do
            [ -e "$entry" ] || continue
            case "${entry##*/}" in
                bin|game) continue ;;
            esac
            rm -rf "$entry"
        done
        log "cleaned run roots under $E2E_ROOT (kept bin, game)"
        ;;
    doctor)
        doctor
        exit $?
        ;;
    *)
        log "usage: naslab_agent.sh {stdio <lease_ms>|run <port> <lease_ms>|drive-root|clean|doctor}"
        exit 2
        ;;
esac

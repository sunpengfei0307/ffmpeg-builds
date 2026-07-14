#!/bin/bash
# Console = quiet stage summary; full detail → $BUILD_LOG
# Expects ROOT_DIR to be set by the entry script before sourcing.

: "${ROOT_DIR:?ROOT_DIR must be set}"
BUILD_LOG="${BUILD_LOG:-${ROOT_DIR}/build/build.log}"
export BUILD_LOG

_log_init() {
  mkdir -p "$(dirname "$BUILD_LOG")"
  : >"$BUILD_LOG"
  {
    echo "===== build start $(date -Is) ====="
    echo "ROOT=$ROOT_DIR"
    echo "ARGS=$*"
    echo "USER=$(id -un 2>/dev/null || true) HOST=$(hostname 2>/dev/null || true)"
    echo "=================================="
  } >>"$BUILD_LOG"
}

_log() { printf '%s\n' "$*" >>"$BUILD_LOG"; }

# Always to log; to console only if VERBOSE=1
_logv() {
  _log "$*"
  [[ -n "${VERBOSE:-}" ]] && printf '%s\n' "$*"
}

_console() { printf '%s\n' "$*"; }

_banner() {
  local title="$1"
  local line
  line="$(printf '=%.0s' {1..56})"
  _console ""
  _console "$line"
  _console "  $title"
  _console "$line"
  _log ""
  _log "$line"
  _log "  $title"
  _log "$line"
}

_ok()   { _console "  [OK]   $*"; _log "[OK] $*"; }
_fail() { _console "  [FAIL] $*"; _log "[FAIL] $*"; }
_skip() { _console "  [SKIP] $*"; _log "[SKIP] $*"; }
_reuse(){ _console "  [REUSE] $*"; _log "[REUSE] $*"; }
_info() { _console "  $*"; _log "[INFO] $*"; }

# Ctrl+C / SIGTERM：终止子进程并退出，避免 tee|while 吞信号后脚本继续跑
_ff_interrupt_setup() {
  export BUILD_INTERRUPTED=0
  trap '_ff_on_interrupt INT' INT
  trap '_ff_on_interrupt TERM' TERM
}

_ff_on_interrupt() {
  local sig="${1:-INT}"
  BUILD_INTERRUPTED=1
  export BUILD_INTERRUPTED
  trap - INT TERM
  printf '\n' >&2
  _console "[INTERRUPT] 收到 ${sig}，正在停止构建（含子进程）…"
  _log "[INTERRUPT] signal=${sig}"
  # 只杀直接/间接子进程，避免 kill 0 误伤同组无关进程
  if command -v pkill >/dev/null 2>&1; then
    pkill -TERM -P $$ 2>/dev/null || true
    sleep 0.3 2>/dev/null || true
    pkill -KILL -P $$ 2>/dev/null || true
  else
    local _p
    for _p in $(ps -o pid= --ppid $$ 2>/dev/null); do
      kill -TERM "$_p" 2>/dev/null || true
    done
    sleep 0.3 2>/dev/null || true
    for _p in $(ps -o pid= --ppid $$ 2>/dev/null); do
      kill -KILL "$_p" 2>/dev/null || true
    done
  fi
  exit 130
}

_ff_exit_if_interrupted() {
  local st="${1:-0}"
  if [[ "${BUILD_INTERRUPTED:-0}" == "1" || "$st" -eq 130 || "$st" -eq 143 ]]; then
    exit 130
  fi
}

# Run command: all output → build.log; return exit code
_run() {
  local desc="${1:-cmd}"
  shift
  _log "+ ($desc) $*"
  local st=0
  local _had_e=0
  [[ $- == *e* ]] && _had_e=1
  set +e
  if [[ -n "${VERBOSE:-}" ]]; then
    set -o pipefail
    "$@" 2>&1 | tee -a "$BUILD_LOG"
    st="${PIPESTATUS[0]:-1}"
    set +o pipefail 2>/dev/null || true
  else
    "$@" >>"$BUILD_LOG" 2>&1
    st=$?
  fi
  [[ "$_had_e" -eq 1 ]] && set -e
  _ff_exit_if_interrupted "$st"
  return "$st"
}

# Run and capture status line for a plugin/stage
_run_plugin() {
  local name="$1"
  shift
  if _run "plugin:$name" "$@"; then
    _ok "$name"
    return 0
  else
    _fail "$name  (详见 build.log)"
    return 1
  fi
}

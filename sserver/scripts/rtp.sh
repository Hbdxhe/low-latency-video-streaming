#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/build/stream_server"

usage() {
  cat <<'EOF'
usage: ./scripts/rtp.sh [null|v4l2] [extra stream_server args...]

Modes:
  null   config/integration_rtp_null.conf, sends to 127.0.0.1:19504
  v4l2   config/integration_rtp_v4l2.conf, sends to 127.0.0.1:19514

Default mode:
  null

Examples:
  ./scripts/rtp.sh
  ./scripts/rtp.sh v4l2
  ./scripts/rtp.sh null --log-level debug
EOF
}

MODE="null"
if [[ $# -gt 0 ]]; then
  case "$1" in
    null|v4l2)
      MODE="$1"
      shift
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
  esac
fi

if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "missing server binary: ${SERVER_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

case "${MODE}" in
  null)
    CONFIG_PATH="${ROOT_DIR}/config/integration_rtp_null.conf"
    ;;
  v4l2)
    CONFIG_PATH="${ROOT_DIR}/config/integration_rtp_v4l2.conf"
    ;;
esac

exec "${SERVER_BIN}" --config "${CONFIG_PATH}" "$@"

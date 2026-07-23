#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/build/stream_server"
CONFIG_PATH="${ROOT_DIR}/config/sclient_tcp.conf"

usage() {
  cat <<'EOF'
usage: ./scripts/tcp.sh [extra stream_server args...]

Starts sserver with the TCP config used for sclient interop:
  config/sclient_tcp.conf

Example:
  ./scripts/tcp.sh
  ./scripts/tcp.sh --log-level debug
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" || "${1:-}" == "help" ]]; then
  usage
  exit 0
fi

if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "missing server binary: ${SERVER_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

exec "${SERVER_BIN}" --config "${CONFIG_PATH}" "$@"

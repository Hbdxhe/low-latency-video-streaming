#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_BIN="${ROOT_DIR}/build/sclient"

usage() {
  cat <<'EOF'
usage: ./scripts/tcp.sh [extra sclient args...]

Starts sclient with the TCP settings used for sserver interop:
  --host 127.0.0.1
  --port 19099
  --transport tcp
  --renderer opengl
  --decoder software

Examples:
  ./scripts/tcp.sh
  ./scripts/tcp.sh --vsync on
  ./scripts/tcp.sh --host 192.168.1.20
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" || "${1:-}" == "help" ]]; then
  usage
  exit 0
fi

if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "missing client binary: ${CLIENT_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

exec "${CLIENT_BIN}" \
  --host 127.0.0.1 \
  --port 19099 \
  --transport tcp \
  --renderer opengl \
  --decoder software \
  "$@"

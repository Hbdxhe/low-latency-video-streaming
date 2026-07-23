#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_BIN="${ROOT_DIR}/build/sclient"

usage() {
  cat <<'EOF'
usage: ./scripts/rtp.sh [null|v4l2|sdp <file>] [extra sclient args...]

Modes:
  null       listen on 127.0.0.1:19504 for config/integration_rtp_null.conf
  v4l2       listen on 127.0.0.1:19514 for config/integration_rtp_v4l2.conf
  sdp <file> connect using SDP file (host/port/payload_type from SDP)

Default mode:
  null

Examples:
  ./scripts/rtp.sh
  ./scripts/rtp.sh v4l2
  ./scripts/rtp.sh sdp demo.sdp
  ./scripts/rtp.sh null --vsync on
EOF
}

MODE="null"
SDP_PATH=""
if [[ $# -gt 0 ]]; then
  case "$1" in
    null|v4l2)
      MODE="$1"
      shift
      ;;
    sdp)
      MODE="sdp"
      shift
      if [[ $# -lt 1 ]]; then
        echo "sdp mode requires a file path" >&2
        usage >&2
        exit 1
      fi
      SDP_PATH="$1"
      shift
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
  esac
fi

if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "missing client binary: ${CLIENT_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

if [[ "${MODE}" == "sdp" ]]; then
  exec "${CLIENT_BIN}" \
    --sdp "${SDP_PATH}" \
    --renderer opengl \
    --decoder software \
    "$@"
fi

case "${MODE}" in
  null)
    PORT="19504"
    ;;
  v4l2)
    PORT="19514"
    ;;
esac

exec "${CLIENT_BIN}" \
  --host 127.0.0.1 \
  --port "${PORT}" \
  --transport rtp \
  --renderer opengl \
  --decoder software \
  "$@"

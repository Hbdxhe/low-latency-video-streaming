#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLIENT_BIN="${ROOT_DIR}/build/sclient"

usage() {
  cat <<'EOF'
usage: ./scripts/udp_profile.sh <balanced|adaptive|resilient> [extra sclient args...]

Profiles:
  balanced   low-latency default for local loopback / wired LAN, port 19100
  adaptive   better jitter tolerance for Wi-Fi / unstable LAN, port 19101
  resilient  larger buffer for lossier links, port 19102

Defaults:
  --renderer opengl
  --decoder software

Examples:
  ./scripts/udp_profile.sh balanced
  ./scripts/udp_profile.sh adaptive --host 192.168.1.20
  ./scripts/udp_profile.sh resilient --vsync on
EOF
}

if [[ $# -lt 1 ]]; then
  usage >&2
  exit 1
fi

PROFILE="$1"
shift

if [[ ! -x "${CLIENT_BIN}" ]]; then
  echo "missing client binary: ${CLIENT_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

COMMON_ARGS=(
  --host 127.0.0.1
  --transport udp
  --renderer opengl
  --decoder software
)

# Profiles only keep arguments that differ from the current sclient defaults.
case "${PROFILE}" in
  balanced)
    PROFILE_ARGS=(
      --port 19100
    )
    ;;
  adaptive)
    PROFILE_ARGS=(
      --port 19101
      --udp-jitter-buffer-strategy auto
      --udp-jitter-buffer-safety 1.5
      --udp-jitter-buffer-max-wait-ms 120
      --udp-jitter-buffer-max-frames 12
      --udp-fec on
      --udp-nack-delay-ms 12
    )
    ;;
  resilient)
    PROFILE_ARGS=(
      --port 19102
      --udp-jitter-buffer-strategy auto
      --udp-jitter-buffer-safety 2.5
      --udp-jitter-buffer-max-wait-ms 250
      --udp-jitter-buffer-max-frames 32
      --udp-fec on
      --udp-nack-delay-ms 12
    )
    ;;
  --help|-h|help)
    usage
    exit 0
    ;;
  *)
    echo "unknown profile: ${PROFILE}" >&2
    usage >&2
    exit 1
    ;;
esac

exec "${CLIENT_BIN}" "${COMMON_ARGS[@]}" "${PROFILE_ARGS[@]}" "$@"

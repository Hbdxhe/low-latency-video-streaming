#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE_SCRIPT="${ROOT_DIR}/scripts/udp_profile.sh"

usage() {
  cat <<'EOF'
usage: ./scripts/udp.sh [balanced|adaptive|resilient] [extra sclient args...]

Profiles:
  balanced   default local loopback / wired LAN profile, port 19100
  adaptive   more jitter tolerant profile, port 19101
  resilient  larger buffer for lossier links, port 19102

Default profile:
  balanced

Examples:
  ./scripts/udp.sh
  ./scripts/udp.sh adaptive
  ./scripts/udp.sh resilient --udp-jitter-buffer off
EOF
}

PROFILE="balanced"
if [[ $# -gt 0 ]]; then
  case "$1" in
    balanced|adaptive|resilient)
      PROFILE="$1"
      shift
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
  esac
fi

if [[ ! -x "${PROFILE_SCRIPT}" ]]; then
  echo "missing profile script: ${PROFILE_SCRIPT}" >&2
  exit 2
fi

exec "${PROFILE_SCRIPT}" "${PROFILE}" "$@"

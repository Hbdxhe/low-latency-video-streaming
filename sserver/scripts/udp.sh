#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_BIN="${ROOT_DIR}/build/stream_server"
CONFIG_PATH="${ROOT_DIR}/config/sclient_udp.conf"

usage() {
  cat <<'EOF'
usage: ./scripts/udp.sh [options]

Options:
  --fec          启用 FEC 前向纠错（丢包恢复）
  --nack         启用 NACK 重传（可靠传输）
  --profile X    使用预设配置 (balanced|adaptive|resilient)
  -h, --help     显示帮助

Examples:
  ./scripts/udp.sh                  # 默认：裸发，最低延迟
  ./scripts/udp.sh --fec            # + FEC 纠错
  ./scripts/udp.sh --nack           # + NACK 重传
  ./scripts/udp.sh --fec --nack     # 全开
  ./scripts/udp.sh --profile resilient  # 高容错预设
EOF
}

FEC="false"
NACK="false"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --fec)
      FEC="true"
      shift
      ;;
    --nack)
      NACK="true"
      shift
      ;;
    --profile)
      case "${2:-}" in
        balanced)
          CONFIG_PATH="${ROOT_DIR}/config/sclient_udp.conf"
          ;;
        adaptive)
          CONFIG_PATH="${ROOT_DIR}/config/sclient_udp_adaptive.conf"
          ;;
        resilient)
          CONFIG_PATH="${ROOT_DIR}/config/sclient_udp_resilient.conf"
          ;;
        *)
          echo "unknown profile: ${2:-}" >&2
          usage >&2
          exit 1
          ;;
      esac
      shift 2
      ;;
    --help|-h|help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "missing server binary: ${SERVER_BIN}" >&2
  echo "build first with: cmake -S . -B build && cmake --build build -j" >&2
  exit 2
fi

FINAL_CONFIG="${CONFIG_PATH}"
if [[ "${FEC}" == "true" || "${NACK}" == "true" ]]; then
  FINAL_CONFIG=$(mktemp /tmp/sserver_udp_XXXXXX.conf)
  cp "${CONFIG_PATH}" "${FINAL_CONFIG}"
  if [[ "${FEC}" == "true" ]]; then
    sed -i 's/^transport\.udp_enable_fec = .*/transport.udp_enable_fec = true/' "${FINAL_CONFIG}"
    if ! grep -q 'transport\.udp_enable_fec' "${FINAL_CONFIG}"; then
      echo 'transport.udp_enable_fec = true' >> "${FINAL_CONFIG}"
    fi
  fi
  if [[ "${NACK}" == "true" ]]; then
    sed -i 's/^transport\.udp_enable_nack = .*/transport.udp_enable_nack = true/' "${FINAL_CONFIG}"
    if ! grep -q 'transport\.udp_enable_nack' "${FINAL_CONFIG}"; then
      echo 'transport.udp_enable_nack = true' >> "${FINAL_CONFIG}"
    fi
  fi
  trap "rm -f '${FINAL_CONFIG}'" EXIT
fi

exec "${SERVER_BIN}" --config "${FINAL_CONFIG}"

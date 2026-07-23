#!/usr/bin/env bash
set -euo pipefail

mode="${1:?usage: run_protocol_benchmark.sh <tcp|udp|rtp> [duration_seconds]}"
duration="${2:-34}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stamp="$(date +%Y%m%d_%H%M%S)"
output_dir="${root}/benchmark-results/${stamp}_${mode}"
mkdir -p "${output_dir}"

server_pid=""
client_pid=""

cleanup() {
    if [[ -n "${client_pid}" ]]; then
        kill -TERM "${client_pid}" 2>/dev/null || true
    fi
    if [[ -n "${server_pid}" ]]; then
        kill -TERM "${server_pid}" 2>/dev/null || true
    fi
    sleep 1
    if [[ -n "${client_pid}" ]]; then
        kill -KILL "${client_pid}" 2>/dev/null || true
        wait "${client_pid}" 2>/dev/null || true
    fi
    if [[ -n "${server_pid}" ]]; then
        kill -KILL "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

case "${mode}" in
    tcp)
        server_cmd=("${root}/sserver/build/stream_server" --config "${root}/sserver/config/sclient_tcp.conf")
        client_cmd=("${root}/sclient/build/sclient" --host 127.0.0.1 --port 19099 --transport tcp --renderer opengl --decoder software --vsync off)
        ;;
    udp)
        server_cmd=("${root}/sserver/build/stream_server" --config "${root}/sserver/config/sclient_udp_resilient.conf")
        client_cmd=("${root}/sclient/build/sclient" --host 127.0.0.1 --port 19102 --transport udp --renderer opengl --decoder software --vsync off --udp-jitter-buffer-strategy auto --udp-jitter-buffer-safety 2.5 --udp-jitter-buffer-max-wait-ms 250 --udp-jitter-buffer-max-frames 32 --udp-fec on --udp-nack on --udp-nack-delay-ms 12)
        ;;
    rtp)
        server_cmd=("${root}/sserver/build/stream_server" --config "${root}/sserver/config/integration_rtp_v4l2.conf")
        client_cmd=("${root}/sclient/build/sclient" --host 127.0.0.1 --port 19514 --transport rtp --renderer opengl --decoder software --vsync off)
        ;;
    *)
        echo "unknown mode: ${mode}" >&2
        exit 2
        ;;
esac

if [[ "${mode}" == "rtp" ]]; then
    stdbuf -oL -eL "${client_cmd[@]}" >"${output_dir}/client.log" 2>&1 &
    client_pid=$!
    sleep 2
    stdbuf -oL -eL "${server_cmd[@]}" >"${output_dir}/server.log" 2>&1 &
    server_pid=$!
else
    stdbuf -oL -eL "${server_cmd[@]}" >"${output_dir}/server.log" 2>&1 &
    server_pid=$!
    sleep 2
    stdbuf -oL -eL "${client_cmd[@]}" >"${output_dir}/client.log" 2>&1 &
    client_pid=$!
fi

printf "second,server_cpu_percent,server_rss_kib,client_cpu_percent,client_rss_kib\n" >"${output_dir}/resources.csv"
for ((second = 1; second <= duration; ++second)); do
    server_sample="$(ps -p "${server_pid}" -o %cpu=,rss= 2>/dev/null | xargs || true)"
    client_sample="$(ps -p "${client_pid}" -o %cpu=,rss= 2>/dev/null | xargs || true)"
    server_cpu="${server_sample%% *}"
    server_rss="${server_sample##* }"
    client_cpu="${client_sample%% *}"
    client_rss="${client_sample##* }"
    printf "%s,%s,%s,%s,%s\n" \
        "${second}" "${server_cpu:-0}" "${server_rss:-0}" "${client_cpu:-0}" "${client_rss:-0}" \
        >>"${output_dir}/resources.csv"
    sleep 1
done

echo "${output_dir}"

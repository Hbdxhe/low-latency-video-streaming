#!/usr/bin/env bash
set -euo pipefail

LAYER="${1:-all}"

cmake -S . -B build
cmake --build build -j

case "${LAYER}" in
  smoke)
    ctest --test-dir build --output-on-failure -L smoke
    ;;
  integration)
    ctest --test-dir build --output-on-failure -L integration
    ;;
  benchmark)
    ctest --test-dir build --output-on-failure -L benchmark
    ;;
  all)
    ctest --test-dir build --output-on-failure
    ;;
  *)
    echo "usage: $0 [smoke|integration|benchmark|all]" >&2
    exit 1
    ;;
esac

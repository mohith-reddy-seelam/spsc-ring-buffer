#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# One command from clean checkout to plots, flame graphs and SUMMARY.md.
#
#   scripts/run_all.sh [results-name] [extra spsc_bench flags...]
#
#   scripts/run_all.sh                         # results/<hostname>/
#   scripts/run_all.sh i5-6500T_win11
#   scripts/run_all.sh quick --quick           # ~15 s smoke run
#
# Environment:
#   PYTHON        interpreter with matplotlib (default: python3 / python)
#   PRODUCER_CPU  CPU for the producer thread (default: second-highest)
#   CONSUMER_CPU  CPU for the consumer thread (default: highest)
#
# Works in Git Bash / MSYS2 on Windows (built-in sampler for flame graphs) and
# on Linux (perf for flame graphs; run as root or set
# kernel.perf_event_paranoid <= 1).
set -euo pipefail

cd "$(dirname "$0")/.."
NAME="${1:-$(hostname)}"
shift || true
OUT="results/${NAME}"
PY="${PYTHON:-$(command -v python3 || command -v python)}"
EXE=""
[[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && EXE=".exe"
BIN="build/release"

pin_flags=()
[[ -n "${PRODUCER_CPU:-}" ]] && pin_flags+=("--producer_cpu=${PRODUCER_CPU}")
[[ -n "${CONSUMER_CPU:-}" ]] && pin_flags+=("--consumer_cpu=${CONSUMER_CPU}")

step() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

step "configure + build (Release)"
cmake --preset release >/dev/null
cmake --build --preset release

step "unit tests"
"${BIN}/spsc_tests${EXE}" --gtest_brief=1

step "benchmarks -> ${OUT}"
mkdir -p "${OUT}"
"${BIN}/spsc_bench${EXE}" --out_dir="${OUT}" "${pin_flags[@]}" \
  --benchmark_out="${OUT}/benchmark.json" --benchmark_out_format=json "$@" \
  | tee "${OUT}/console.txt"

step "flame graphs"
mkdir -p "${OUT}/flamegraphs"
for q in ring mutex_queue; do
  title="Lock-free SPSC ring"
  [[ $q == mutex_queue ]] && title="std::mutex + std::queue"
  raw="${OUT}/flamegraphs/throughput_${q}.raw.txt"
  if [[ -n "${EXE}" ]]; then
    "${BIN}/spsc_profile${EXE}" --queue="$q" --workload=throughput --seconds=5 "${pin_flags[@]}" \
      --out="${raw}"
    "${PY}" tools/flamegraph.py "${BIN}/spsc_profile${EXE}" "${raw}" \
      "${OUT}/flamegraphs/throughput_${q}.svg" --title "${title}: saturated throughput"
  elif command -v perf >/dev/null; then
    perf record -F 1999 -g --call-graph dwarf -o "${OUT}/flamegraphs/${q}.perf.data" -- \
      "${BIN}/spsc_profile" --queue="$q" --workload=throughput --seconds=5 --no-sampler \
      "${pin_flags[@]}"
    [[ -d third_party/FlameGraph ]] ||
      git clone --depth 1 https://github.com/brendangregg/FlameGraph.git third_party/FlameGraph
    perf script -i "${OUT}/flamegraphs/${q}.perf.data" \
      | third_party/FlameGraph/stackcollapse-perf.pl \
      | third_party/FlameGraph/flamegraph.pl --title "${title}: saturated throughput" \
      > "${OUT}/flamegraphs/throughput_${q}.svg"
  else
    echo "perf not found; skipping flame graphs"
  fi
done

step "plots + summary"
"${PY}" tools/analyze.py "${OUT}"
echo
echo "done: ${OUT}/SUMMARY.md, ${OUT}/plots/, ${OUT}/flamegraphs/"

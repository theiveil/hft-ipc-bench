#!/usr/bin/env bash
# run_bench.sh — Full benchmark sweep + analysis
#
# Tests all 5 IPC implementations across:
#   N_MESSAGES : 100_000 / 1_000_000 / 10_000_000
#   N_CONSUMERS: 1 / 2 / 4
#   TARGET_MPS : 2_000_000 / 5_000_000 / 10_000_000
#
# All result files land in  <project>/output/
# with names like  latency_shm_N1000000_C4_c0.bin
#
# Plots are written to  <project>/output/plots/
#
# Usage:
#   ./run_bench.sh [--ipc shm|uds|zmq|grpc|redis] [--msgs M] [--cons C] [--mps MPS]
#   (flags can be repeated or omitted for defaults)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${SCRIPT_DIR}/output"
PLOTS_DIR="${OUT_DIR}/plots"

# ── Defaults ──────────────────────────────────────────────────────────────────
IPC_LIST=(shm uds zmq grpc redis)
MSG_LIST=(100000 1000000 10000000)
CONS_LIST=(1 2 4)
MPS_LIST=(2000000 5000000 10000000)

# ── CLI overrides ─────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ipc)  IPC_LIST=("$2");  shift 2 ;;
        --msgs) MSG_LIST=("$2");  shift 2 ;;
        --cons) CONS_LIST=("$2"); shift 2 ;;
        --mps)  MPS_LIST=("$2");  shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

mkdir -p "$OUT_DIR" "$PLOTS_DIR"

# Clean any IPC files left by a previous (possibly crashed) run
rm -f /dev/shm/bench_shm_* 2>/dev/null || true
rm -f /tmp/bench_uds* 2>/dev/null || true

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GRN='\033[0;32m'; YEL='\033[1;33m'; CYN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYN}[INFO]${NC}  $*"; }
ok()    { echo -e "${GRN}[OK]${NC}    $*"; }
err()   { echo -e "${RED}[ERR]${NC}   $*"; }

# ── Kill background jobs on exit ──────────────────────────────────────────────
trap 'jobs -p | xargs -r kill 2>/dev/null || true' EXIT

# ── Wait for N consumer .tp files to appear ───────────────────────────────────
# Usage: wait_consumers <ipc> <n_msgs> <n_cons> <target_mps> [timeout] [pid...]
# Fails fast if all consumer PIDs have already exited without writing .tp.
wait_consumers() {
    local ipc="$1" n_msgs="$2" n_cons="$3" target_mps="$4" timeout="${5:-300}"
    shift 5
    local consumer_pids=("$@")
    info "Waiting for ${n_cons} consumer(s) to finish..."
    local tag="_N${n_msgs}_C${n_cons}_MPS${target_mps}"
    for (( i=0; i < timeout*10; i++ )); do
        local done=0
        for (( c=0; c < n_cons; c++ )); do
            [[ -f "${OUT_DIR}/bench_${ipc}${tag}_c${c}.tp" ]] && (( done++ )) || true
        done
        [[ $done -ge $n_cons ]] && return 0

        # Fail fast: if every consumer PID has already exited and we still
        # don't have all .tp files, something went wrong (crash / exception).
        if [[ ${#consumer_pids[@]} -gt 0 ]]; then
            local alive=0
            for pid in "${consumer_pids[@]}"; do
                kill -0 "$pid" 2>/dev/null && (( alive += 1 )) || true
            done
            if [[ $alive -eq 0 ]]; then
                # Give the OS a moment to flush any in-flight writes, then recheck
                sleep 0.2
                done=0
                for (( c=0; c < n_cons; c++ )); do
                    [[ -f "${OUT_DIR}/bench_${ipc}${tag}_c${c}.tp" ]] && (( done++ )) || true
                done
                [[ $done -ge $n_cons ]] && return 0
                err "All consumers exited early (${done}/${n_cons} .tp files written)"
                return 1
            fi
        fi
        sleep 0.1
    done
    err "Consumers did not finish within ${timeout}s"
    return 1
}

# ════════════════════════════════════════════════════════════════════════════
# SHM
# ════════════════════════════════════════════════════════════════════════════
run_shm() {
    local n_msgs="$1" n_cons="$2" target_mps="$3"
    local tag="_N${n_msgs}_C${n_cons}_MPS${target_mps}"
    info "── SHM  N=${n_msgs}  C=${n_cons}  MPS=${target_mps} ──"

    # Clean stale output files and stale SHM segment for this combo
    rm -f "${OUT_DIR}"/bench_shm${tag}* "${OUT_DIR}"/latency_shm${tag}*
    rm -f "/dev/shm/bench_shm${tag}"

    local pids=()
    for (( c=0; c<n_cons; c++ )); do
        "${SCRIPT_DIR}/shm/bench_consumer" "$c" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps" &
        pids+=($!)
    done

    "${SCRIPT_DIR}/shm/bench_producer" "$n_cons" "$n_msgs" "$OUT_DIR" "$target_mps"

    wait_consumers shm "$n_msgs" "$n_cons" "$target_mps" 300 "${pids[@]}"
    wait "${pids[@]}" 2>/dev/null || true
    ok "SHM done"
}

# ════════════════════════════════════════════════════════════════════════════
# UDS
# ════════════════════════════════════════════════════════════════════════════
run_uds() {
    local n_msgs="$1" n_cons="$2" target_mps="$3"
    local tag="_N${n_msgs}_C${n_cons}_MPS${target_mps}"
    info "── UDS  N=${n_msgs}  C=${n_cons}  MPS=${target_mps} ──"

    rm -f "${OUT_DIR}"/bench_uds${tag}* "${OUT_DIR}"/latency_uds${tag}*
    rm -f /tmp/bench_uds${tag}*

    local pids=()
    for (( c=0; c<n_cons; c++ )); do
        "${SCRIPT_DIR}/uds/consumer" "$c" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps" &
        pids+=($!)
    done

    "${SCRIPT_DIR}/uds/producer" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps"

    wait_consumers uds "$n_msgs" "$n_cons" "$target_mps" 300 "${pids[@]}"
    wait "${pids[@]}" 2>/dev/null || true
    rm -f /tmp/bench_uds${tag}*
    ok "UDS done"
}

# ════════════════════════════════════════════════════════════════════════════
# ZeroMQ
# ════════════════════════════════════════════════════════════════════════════
run_zmq() {
    local n_msgs="$1" n_cons="$2" target_mps="$3"
    local tag="_N${n_msgs}_C${n_cons}_MPS${target_mps}"
    info "── ZMQ  N=${n_msgs}  C=${n_cons}  MPS=${target_mps} ──"

    rm -f "${OUT_DIR}"/bench_zmq${tag}* "${OUT_DIR}"/latency_zmq${tag}*

    local pids=()
    for (( c=0; c<n_cons; c++ )); do
        "${SCRIPT_DIR}/zeroMQ/consumer" "$c" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps" &
        pids+=($!)
    done

    sleep 1.5   # let subscribers connect before producer starts

    "${SCRIPT_DIR}/zeroMQ/producer" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps"

    wait_consumers zmq "$n_msgs" "$n_cons" "$target_mps" 300
    wait "${pids[@]}" 2>/dev/null || true
    ok "ZMQ done"
}

# ════════════════════════════════════════════════════════════════════════════
# gRPC
# ════════════════════════════════════════════════════════════════════════════
run_grpc() {
    local n_msgs="$1" n_cons="$2" target_mps="$3"
    local tag="_N${n_msgs}_C${n_cons}_MPS${target_mps}"
    info "── gRPC  N=${n_msgs}  C=${n_cons}  MPS=${target_mps} ──"

    rm -f "${OUT_DIR}"/bench_grpc${tag}* "${OUT_DIR}"/latency_grpc${tag}* /tmp/bench_grpc.sock

    # Start server first — socket file must exist before clients connect
    "${SCRIPT_DIR}/grpc/bench_server" "$n_cons" "$n_msgs" "$OUT_DIR" "$target_mps" &
    local server_pid=$!

    # Wait for the Unix socket to appear (up to 10 s)
    # NOTE: use "waited += 1" — (( n++ )) returns 1 when n==0, which trips set -e
    local waited=0
    until [[ -S /tmp/bench_grpc.sock ]]; do
        sleep 0.1
        (( waited += 1 ))
        if (( waited > 100 )); then
            err "gRPC server socket did not appear after 10s"
            kill "$server_pid" 2>/dev/null || true
            return 1
        fi
    done
    info "gRPC socket ready (${waited}00 ms)"

    # Now start clients
    local pids=()
    for (( c=0; c<n_cons; c++ )); do
        "${SCRIPT_DIR}/grpc/bench_client" "$c" "$n_cons" "$n_msgs" "$OUT_DIR" "$target_mps" &
        pids+=($!)
    done

    wait_consumers grpc "$n_msgs" "$n_cons" "$target_mps" 300
    wait "${pids[@]}" 2>/dev/null || true
    # Server shuts itself down after last consumer finishes; kill is a safety net
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    rm -f /tmp/bench_grpc.sock
    ok "gRPC done"
}

# ════════════════════════════════════════════════════════════════════════════
# Redis
# ════════════════════════════════════════════════════════════════════════════
start_redis() {
    if pgrep -f "redis-server.*redis_bench.sock" &>/dev/null; then
        info "Redis already running"; return
    fi
    info "Starting Redis..."
    redis-server "${SCRIPT_DIR}/Redis/redis.conf" --daemonize yes
    for (( i=0; i<50; i++ )); do
        [[ -S /tmp/redis_bench.sock ]] && return 0
        sleep 0.2
    done
    err "Redis socket did not appear"; return 1
}

stop_redis() {
    redis-cli -s /tmp/redis_bench.sock shutdown nosave 2>/dev/null || true
    rm -f /tmp/redis_bench.sock
}

run_redis() {
    local n_msgs="$1" n_cons="$2" target_mps="$3"
    local tag="_N${n_msgs}_C${n_cons}_MPS${target_mps}"
    info "── Redis  N=${n_msgs}  C=${n_cons}  MPS=${target_mps} ──"

    rm -f "${OUT_DIR}"/bench_redis${tag}* "${OUT_DIR}"/latency_redis${tag}*
    start_redis

    local pids=()
    for (( c=0; c<n_cons; c++ )); do
        "${SCRIPT_DIR}/Redis/consumer" "$c" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps" &
        pids+=($!)
    done

    sleep 1

    "${SCRIPT_DIR}/Redis/producer" "$n_msgs" "$n_cons" "$OUT_DIR" "$target_mps"

    wait_consumers redis "$n_msgs" "$n_cons" "$target_mps" 300
    wait "${pids[@]}" 2>/dev/null || true
    stop_redis
    ok "Redis done"
}

# ════════════════════════════════════════════════════════════════════════════
# Main sweep
# ════════════════════════════════════════════════════════════════════════════
total=$(( ${#IPC_LIST[@]} * ${#MSG_LIST[@]} * ${#CONS_LIST[@]} * ${#MPS_LIST[@]} ))
run=0

for ipc in "${IPC_LIST[@]}"; do
    for n_msgs in "${MSG_LIST[@]}"; do
        for n_cons in "${CONS_LIST[@]}"; do
            for target_mps in "${MPS_LIST[@]}"; do
                (( run++ )) || true
                echo ""
                echo -e "${YEL}════ Run ${run}/${total}: ${ipc^^}  N=${n_msgs}  C=${n_cons}  MPS=${target_mps} ════${NC}"
                case "$ipc" in
                    shm)   run_shm   "$n_msgs" "$n_cons" "$target_mps" ;;
                    uds)   run_uds   "$n_msgs" "$n_cons" "$target_mps" ;;
                    zmq)   run_zmq   "$n_msgs" "$n_cons" "$target_mps" ;;
                    grpc)  run_grpc  "$n_msgs" "$n_cons" "$target_mps" ;;
                    redis) run_redis "$n_msgs" "$n_cons" "$target_mps" ;;
                    *)     err "Unknown IPC: $ipc" ;;
                esac
            done
        done
    done
done

# ── Final analysis & plots ────────────────────────────────────────────────────
echo ""
info "Running analyze.py → plots in ${PLOTS_DIR}"

python3 "${SCRIPT_DIR}/analyze.py" \
    --out-dir   "$OUT_DIR"  \
    --plots-dir "$PLOTS_DIR" \
    --ipc-list  "${IPC_LIST[@]}" \
    --msg-list  "${MSG_LIST[@]}" \
    --cons-list "${CONS_LIST[@]}" \
    --mps-list  "${MPS_LIST[@]}"

ok "All done. Results: ${OUT_DIR}   Plots: ${PLOTS_DIR}"

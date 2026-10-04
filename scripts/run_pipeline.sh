#!/usr/bin/env bash
# run_pipeline.sh - one automated replay -> receive -> analyse experiment.
#
# Normally called from the Makefile. All settings come from the environment:
#   MODE        rev3 (2 threads + book + flight recorder) | recv (single thread)
#   NAME        short run name (used in the results directory)
#   DATA COUNT RATE PORT RCVBUF SAMPLE REPLAY_CPU RX_CPU PARSE_CPU LOG_CPU PY TAG
#
# Produces results/<timestamp>_<NAME>/{meta.txt,receiver.log,replay.log,
# analyze.txt,trace.bin} and appends one row to results/summary.csv.
set -euo pipefail

cd "$(dirname "$0")/.."                      # project root
ROOT=$PWD
B=$ROOT/build

MODE=${MODE:-rev3}
NAME=${NAME:-run}
DATA=${DATA:-itch/test1.itch}
COUNT=${COUNT:-5000000}
RATE=${RATE:-100000}
PORT=${PORT:-9999}
RCVBUF=${RCVBUF:-33554432}
SAMPLE=${SAMPLE:-1}
REPLAY_CPU=${REPLAY_CPU:-2}
RX_CPU=${RX_CPU:-3}
PARSE_CPU=${PARSE_CPU:-4}
LOG_CPU=${LOG_CPU:-5}
PY=${PY:-python3}
TAG=${TAG:-}

die() { echo "ERROR: $*" >&2; exit 1; }

# ---- pre-flight checks -----------------------------------------------------------
[[ -f $DATA ]] || die "data file $DATA not found"
if [[ $(head -c 2 "$DATA" | od -An -tx1 | tr -d ' ') == "1f8b" ]]; then
    die "$DATA is gzip-compressed; run: zcat $DATA > ${DATA%.gz}.itch"
fi
case $MODE in
  rev3) RX_BIN=$B/itch_rev3 ;;
  recv) RX_BIN=$B/itch_recv ;;
  *)    die "unknown MODE=$MODE" ;;
esac
[[ -x $RX_BIN ]]           || die "$RX_BIN not built (run make)"
[[ -x $B/itch_replay ]]    || die "$B/itch_replay not built (run make)"
if command -v ss >/dev/null && ss -uln | grep -q ":$PORT\b"; then
    die "UDP port $PORT already in use (old receiver still running? pkill itch_rev3)"
fi
ncpu=$(nproc)
for c in $REPLAY_CPU $RX_CPU $PARSE_CPU $LOG_CPU; do
    (( c < ncpu )) || die "CPU $c does not exist (nproc=$ncpu)"
done

STAMP=$(date +%Y%m%d_%H%M%S)
OUT=$ROOT/results/${STAMP}_${NAME}${TAG:+_$TAG}
mkdir -p "$OUT"

# ---- metadata: everything needed to reproduce / compare the run -------------------------
{
  echo "date        : $(date -Is)"
  echo "name        : $NAME ${TAG}"
  echo "mode        : $MODE"
  echo "data        : $DATA ($(stat -c %s "$DATA") bytes)"
  echo "count/rate  : $COUNT msgs @ $([[ $RATE == 0 ]] && echo max || echo "$RATE msg/s")"
  echo "rcvbuf      : $RCVBUF   sample: 1/$SAMPLE"
  echo "cpus        : replay=$REPLAY_CPU rx=$RX_CPU parse=$PARSE_CPU log=$LOG_CPU"
  echo "flags       : ${BUILD_FLAGS:-unknown}"
  echo "git         : $(git rev-parse --short HEAD 2>/dev/null || echo n/a)$(git diff --quiet 2>/dev/null || echo ' (dirty)')"
  echo "kernel      : $(uname -r)"
  echo "cpu model   : $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)"
  echo "virt        : $(systemd-detect-virt 2>/dev/null || echo unknown)"
  echo "load        : $(cut -d' ' -f1-3 /proc/loadavg)"
  echo "rmem_max    : $(cat /proc/sys/net/core/rmem_max 2>/dev/null || echo n/a)"
} > "$OUT/meta.txt"

# ---- start receiver, wait until it is listening ------------------------------------
RX_ARGS=("$PORT")
[[ $RCVBUF != 0 ]] && RX_ARGS+=(--rcvbuf "$RCVBUF")
if [[ $MODE == rev3 ]]; then
    RX_ARGS+=(--rx-cpu "$RX_CPU" --parse-cpu "$PARSE_CPU" --log-cpu "$LOG_CPU"
              --mlock --trace "$OUT/trace.bin" --sample "$SAMPLE")
else
    RX_ARGS+=(--cpu "$RX_CPU")
fi

stdbuf -oL -eL "$RX_BIN" "${RX_ARGS[@]}" > "$OUT/receiver.log" 2>&1 &
RX_PID=$!
trap 'kill $RX_PID 2>/dev/null || true' EXIT

for _ in $(seq 100); do                       # up to 10 s
    grep -q "listening" "$OUT/receiver.log" 2>/dev/null && break
    kill -0 $RX_PID 2>/dev/null || { cat "$OUT/receiver.log"; die "receiver exited early"; }
    sleep 0.1
done
grep -q "listening" "$OUT/receiver.log" || die "receiver never reported 'listening'"

# ---- replay ------------------------------------------------------------------------
echo ">>> [$NAME] replaying $COUNT msgs @ $([[ $RATE == 0 ]] && echo max || echo "$RATE/s") ..."
REPLAY_ARGS=("$DATA" 127.0.0.1 "$PORT" --count "$COUNT" --cpu "$REPLAY_CPU")
[[ $RATE != 0 ]] && REPLAY_ARGS+=(--rate "$RATE")
"$B/itch_replay" "${REPLAY_ARGS[@]}" > "$OUT/replay.log" 2>&1

wait $RX_PID || true                          # receiver exits after 2 s idle
trap - EXIT

# ---- analyse -----------------------------------------------------------------------
if [[ $MODE == rev3 && -f $OUT/trace.bin ]]; then
    "$PY" reciever/analyze.py "$OUT/trace.bin" > "$OUT/analyze.txt" 2>&1 || true
fi

# ---- extract key numbers -----------------------------------------------------------
num() { grep -m1 "$1" "$2" 2>/dev/null | sed -E "s/.*$1[^0-9]*([0-9]+).*/\1/" || true; }
sent=$(num 'sent' "$OUT/replay.log")
recv_n=$(num 'received' "$OUT/receiver.log")
lost=$(num 'lost' "$OUT/receiver.log")
depth=$(grep -m1 'max_depth' "$OUT/receiver.log" | sed -E 's/.*max_depth ~ ([0-9]+).*/\1/' || true)
missing=$(grep -m1 'missing_ref=' "$OUT/receiver.log" | sed -E 's/.*missing_ref=([0-9]+).*/\1/' || true)
p50=; p99=; p999=; pmax=
if [[ -f $OUT/analyze.txt ]]; then
    read -r p50 p90 p99 p999 p9999 pmax _ < <(grep -m1 '^total' "$OUT/analyze.txt" \
        | sed -E 's/^total[^)]*\)//') || true
fi

status=PASS; why=
[[ ${sent:-x} == "${COUNT}" ]]          || { status=FAIL; why+=" sent!=count"; }
[[ ${recv_n:-x} == "${sent:-y}" ]]      || { status=FAIL; why+=" received!=sent"; }
[[ ${lost:-x} == 0 ]]                   || { status=FAIL; why+=" lost=$lost"; }
if [[ $MODE == rev3 ]]; then
    grep -q "== received, OK" "$OUT/receiver.log" || { status=FAIL; why+=" parsed!=received"; }
    [[ ${missing:-x} == 0 ]]             || { status=FAIL; why+=" missing_ref=$missing"; }
fi
grep -q "first seq" "$OUT/receiver.log" && { status=FAIL; why+=" late-start"; }

# ---- summary ---------------------------------------------------------------------------
SUM=$ROOT/results/summary.csv
[[ -f $SUM ]] || echo "time,name,mode,rate,count,received,lost,max_depth,p50_ns,p99_ns,p999_ns,max_ns,status,dir" > "$SUM"
echo "$STAMP,$NAME${TAG:+_$TAG},$MODE,$RATE,$COUNT,${recv_n:-},${lost:-},${depth:-},${p50:-},${p99:-},${p999:-},${pmax:-},$status,$(basename "$OUT")" >> "$SUM"

echo "<<< [$NAME] $status${why:+ ($why )}  received=${recv_n:-?} lost=${lost:-?} depth=${depth:--}" \
     "total p50/p99/p99.9/max = ${p50:--}/${p99:--}/${p999:--}/${pmax:--} ns"
echo "    results: $OUT"
[[ $status == PASS ]]


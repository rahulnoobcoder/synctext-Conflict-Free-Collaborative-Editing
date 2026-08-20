#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDITOR_BIN="$ROOT_DIR/editor"

if [[ ! -x "$EDITOR_BIN" ]]; then
  echo "editor binary not found. Run: make"
  exit 1
fi

USER1="user_1"
USER2="user_2"
USER3="user_3"

PORT1=9101
PORT2=9102
PORT3=9103

DOC1="$ROOT_DIR/${USER1}_doc.txt"
DOC2="$ROOT_DIR/${USER2}_doc.txt"
DOC3="$ROOT_DIR/${USER3}_doc.txt"

cleanup() {
  [[ -n "${PID1:-}" ]] && kill "$PID1" 2>/dev/null || true
  [[ -n "${PID2:-}" ]] && kill "$PID2" 2>/dev/null || true
  [[ -n "${PID3:-}" ]] && kill "$PID3" 2>/dev/null || true
  rm -f "$ROOT_DIR/peers.conf"
}
trap cleanup EXIT

ITERATIONS=${ITERATIONS:-5}
RESULTS_FILE="$ROOT_DIR/test_results.txt"

cat > "$ROOT_DIR/peers.conf" <<EOF
127.0.0.1:$PORT1
127.0.0.1:$PORT2
127.0.0.1:$PORT3
EOF

run_round() {
  local round="$1"
  : > "$DOC1"
  : > "$DOC2"
  : > "$DOC3"

  "$EDITOR_BIN" "$USER1" "$PORT1" >/tmp/syntext_${USER1}.log 2>&1 & PID1=$!
  "$EDITOR_BIN" "$USER2" "$PORT2" >/tmp/syntext_${USER2}.log 2>&1 & PID2=$!
  "$EDITOR_BIN" "$USER3" "$PORT3" >/tmp/syntext_${USER3}.log 2>&1 & PID3=$!

  sleep 1

  for i in $(seq 1 10); do
    echo "seed-$i" >> "$DOC1"
    echo "seed-$i" >> "$DOC2"
    echo "seed-$i" >> "$DOC3"
  done

  echo "Round $round: running stress edits..."
  local start_ns
  start_ns=$(date +%s%N)

  case "$round" in
    1)
      for i in $(seq 1 50); do
        echo "A:$i $(head -c 8 /dev/urandom | base64 | tr -d '=')" >> "$DOC1"
        echo "B:$i $(head -c 8 /dev/urandom | base64 | tr -d '=')" >> "$DOC2"
        echo "C:$i $(head -c 8 /dev/urandom | base64 | tr -d '=')" >> "$DOC3"
        if (( i % 10 == 0 )); then
          sleep 0.1
        fi
      done

      printf "conflict-line\n" >> "$DOC1"
      printf "conflict-line\n" >> "$DOC2"
      printf "conflict-line\n" >> "$DOC3"
      sleep 0.2
      perl -0777 -i -pe 's/conflict-line/alpha-edit/g' "$DOC1"
      perl -0777 -i -pe 's/conflict-line/beta-edit/g' "$DOC2"
      perl -0777 -i -pe 's/conflict-line/gamma-edit/g' "$DOC3"

      for i in $(seq 1 20); do
        sed -i '1d' "$DOC1"
        sed -i '1d' "$DOC2"
        sed -i '1d' "$DOC3"
        if (( i % 5 == 0 )); then
          sleep 0.1
        fi
      done
      ;;
    2)
      for i in $(seq 1 30); do
        echo "X:$i $(head -c 6 /dev/urandom | base64 | tr -d '=')" >> "$DOC1"
        echo "Y:$i $(head -c 6 /dev/urandom | base64 | tr -d '=')" >> "$DOC2"
        echo "Z:$i $(head -c 6 /dev/urandom | base64 | tr -d '=')" >> "$DOC3"
      done

      sleep 0.2
      perl -0777 -i -pe 's/seed-5/seed-5-alpha/g' "$DOC1"
      perl -0777 -i -pe 's/seed-5/seed-5-beta/g' "$DOC2"
      perl -0777 -i -pe 's/seed-5/seed-5-gamma/g' "$DOC3"
      sed -i '3i INSERT-A' "$DOC1"
      sed -i '3i INSERT-B' "$DOC2"
      sed -i '3i INSERT-C' "$DOC3"
      ;;
    3)
      for i in $(seq 1 20); do
        echo "M:$i $(head -c 4 /dev/urandom | base64 | tr -d '=')" >> "$DOC1"
        echo "N:$i $(head -c 4 /dev/urandom | base64 | tr -d '=')" >> "$DOC2"
        echo "O:$i $(head -c 4 /dev/urandom | base64 | tr -d '=')" >> "$DOC3"
        perl -0777 -i -pe 's/seed-1/seed-1-A/g' "$DOC1"
        perl -0777 -i -pe 's/seed-1/seed-1-B/g' "$DOC2"
        perl -0777 -i -pe 's/seed-1/seed-1-C/g' "$DOC3"
      done
      ;;
    4)
      for i in $(seq 1 40); do
        sed -i "1s/.*/top-A-$i/" "$DOC1"
        sed -i "1s/.*/top-B-$i/" "$DOC2"
        sed -i "1s/.*/top-C-$i/" "$DOC3"
        if (( i % 8 == 0 )); then
          sleep 0.05
        fi
      done
      ;;
    *)
      for i in $(seq 1 200); do
        echo "bulk-$i" >> "$DOC1"
        echo "bulk-$i" >> "$DOC2"
        echo "bulk-$i" >> "$DOC3"
      done
      sleep 0.2
      for i in $(seq 1 30); do
        sed -i '5d' "$DOC1"
        sed -i '6d' "$DOC2"
        sed -i '7d' "$DOC3"
        if (( i % 10 == 0 )); then
          sleep 0.05
        fi
      done
      ;;
  esac

  stable=0
  prev=""
  for _ in $(seq 1 80); do
    h1=$(sha256sum "$DOC1" | awk '{print $1}')
    h2=$(sha256sum "$DOC2" | awk '{print $1}')
    h3=$(sha256sum "$DOC3" | awk '{print $1}')
    if [[ "$h1" == "$h2" && "$h2" == "$h3" ]]; then
      if [[ "$h1" == "$prev" ]]; then
        stable=$((stable + 1))
      else
        stable=1
        prev="$h1"
      fi
    else
      stable=0
      prev=""
    fi
    if (( stable >= 5 )); then
      break
    fi
    sleep 0.2
  done

  local end_ns
  end_ns=$(date +%s%N)
  local elapsed_ms
  elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))

  if (( stable >= 5 )); then
    echo "Round $round: Converged: yes" | tee -a "$RESULTS_FILE"
  else
    echo "Round $round: Converged: no" | tee -a "$RESULTS_FILE"
  fi
  echo "Round $round: Elapsed ms: $elapsed_ms" | tee -a "$RESULTS_FILE"

  # ---- content-correctness check ----------------------------------------
  # Collect every unique line actually written by any user during this round.
  # The merged document must contain each such line at least once and must
  # not contain any line that appears more than twice (duplication is the
  # canonical symptom of the index-corruption bug).
  local correct=1
  local dup_found=0
  while IFS= read -r expected_line; do
    # skip blank lines (empty lines in the file are normal padding)
    [[ -z "$expected_line" ]] && continue
    if ! grep -qxF -- "$expected_line" "$DOC1"; then
      echo "Round $round: CORRECTNESS FAIL — line missing from merged doc: [$expected_line]" \
        | tee -a "$RESULTS_FILE"
      correct=0
    fi
    count=$(grep -cxF -- "$expected_line" "$DOC1" 2>/dev/null || true)
    if (( count > 2 )); then
      echo "Round $round: CORRECTNESS FAIL — line appears $count times (duplication): [$expected_line]" \
        | tee -a "$RESULTS_FILE"
      dup_found=1
      correct=0
    fi
  done < <(sort -u "$DOC1" "$DOC2" "$DOC3" 2>/dev/null)
  if (( correct == 1 )); then
    echo "Round $round: Correctness: ok" | tee -a "$RESULTS_FILE"
  else
    echo "Round $round: Correctness: FAIL" | tee -a "$RESULTS_FILE"
  fi


  kill "$PID1" "$PID2" "$PID3" 2>/dev/null || true
  wait "$PID1" "$PID2" "$PID3" 2>/dev/null || true
}

: > "$RESULTS_FILE"
for round in $(seq 1 "$ITERATIONS"); do
  run_round "$round"
done

echo "Sample tail of final converged document:" | tee -a "$RESULTS_FILE"
tail -n 5 "$DOC1" | tee -a "$RESULTS_FILE" || true

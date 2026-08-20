#!/usr/bin/env bash
# test_headless.sh — Three-way headless stress test for SyncText.
# Injects InsertChar/DeleteChar ops via stdin to three editor processes
# and asserts that all three converge to the same correct content.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$ROOT/editor"

if [[ ! -x "$BIN" ]]; then
  echo "ERROR: editor binary not found. Run: make" >&2
  exit 1
fi

PORT1=19101; PORT2=19102; PORT3=19103
U1=hl_u1; U2=hl_u2; U3=hl_u3
RESULTS="$ROOT/headless_results.txt"
: > "$RESULTS"

cleanup() {
  [[ -n "${PID1:-}" ]] && kill "$PID1" 2>/dev/null || true
  [[ -n "${PID2:-}" ]] && kill "$PID2" 2>/dev/null || true
  [[ -n "${PID3:-}" ]] && kill "$PID3" 2>/dev/null || true
  rm -f "$ROOT/peers.conf"
  rm -f "$ROOT"/*_doc.txt
}
trap cleanup EXIT

# Write manual peer config.
cat > "$ROOT/peers.conf" <<EOF
127.0.0.1:$PORT1
127.0.0.1:$PORT2
127.0.0.1:$PORT3
EOF

# ---------------------------------------------------------------------------
# run_round: inject characters, wait for convergence, check correctness.
# ---------------------------------------------------------------------------
run_round() {
  local round="$1"
  local chars_A="$2"   # e.g. "hello"
  local chars_B="$3"   # e.g. "world"
  local chars_C="$4"   # e.g. "!!!"

  rm -f "$ROOT"/*_doc.txt

  # FIFOs for stdin injection.
  local FIFO1; FIFO1=$(mktemp -u /tmp/hl_fifo1_XXXX)
  local FIFO2; FIFO2=$(mktemp -u /tmp/hl_fifo2_XXXX)
  local FIFO3; FIFO3=$(mktemp -u /tmp/hl_fifo3_XXXX)
  mkfifo "$FIFO1" "$FIFO2" "$FIFO3"

  # Start editor processes in headless mode.
  "$BIN" "$U1" "$PORT1" --headless < "$FIFO1" > /tmp/hl_out1.txt 2>/tmp/hl_err1.txt &
  PID1=$!
  "$BIN" "$U2" "$PORT2" --headless < "$FIFO2" > /tmp/hl_out2.txt 2>/tmp/hl_err2.txt &
  PID2=$!
  "$BIN" "$U3" "$PORT3" --headless < "$FIFO3" > /tmp/hl_out3.txt 2>/tmp/hl_err3.txt &
  PID3=$!

  # Give processes time to connect to each other.
  sleep 1

  # Send InsertChar commands.
  (
    for c in $(echo "$chars_A" | fold -w1); do
      echo "I $c"
    done
    sleep 3
    echo "P"  # print final doc
  ) > "$FIFO1" &

  (
    for c in $(echo "$chars_B" | fold -w1); do
      echo "I $c"
    done
    sleep 3
    echo "P"
  ) > "$FIFO2" &

  (
    for c in $(echo "$chars_C" | fold -w1); do
      echo "I $c"
    done
    sleep 3
    echo "P"
  ) > "$FIFO3" &

  # Wait for all processes to settle and print.
  sleep 4

  # Read printed outputs.
  local out1; out1=$(cat /tmp/hl_out1.txt 2>/dev/null || echo "")
  local out2; out2=$(cat /tmp/hl_out2.txt 2>/dev/null || echo "")
  local out3; out3=$(cat /tmp/hl_out3.txt 2>/dev/null || echo "")

  # Kill processes.
  kill "$PID1" "$PID2" "$PID3" 2>/dev/null || true
  wait "$PID1" "$PID2" "$PID3" 2>/dev/null || true
  rm -f "$FIFO1" "$FIFO2" "$FIFO3"

  # Correctness assertions.
  local all_chars="${chars_A}${chars_B}${chars_C}"
  local converged=1
  local correct=1

  # All three outputs must be identical.
  if [[ "$out1" != "$out2" || "$out2" != "$out3" ]]; then
    echo "Round $round: CONVERGENCE FAIL" | tee -a "$RESULTS"
    echo "  u1: [$out1]" | tee -a "$RESULTS"
    echo "  u2: [$out2]" | tee -a "$RESULTS"
    echo "  u3: [$out3]" | tee -a "$RESULTS"
    converged=0
  else
    echo "Round $round: Converged: yes" | tee -a "$RESULTS"
  fi

  # Every typed character must appear in the merged output.
  for c in $(echo "$all_chars" | fold -w1); do
    if [[ "$out1" != *"$c"* ]]; then
      echo "Round $round: CORRECTNESS FAIL — char '$c' missing from merged doc" | tee -a "$RESULTS"
      correct=0
    fi
  done

  # Check for duplication: each unique input char should appear at most
  # len(A)+len(B)+len(C) times total in the merged result.
  local expected_len=$(( ${#chars_A} + ${#chars_B} + ${#chars_C} ))
  local actual_len=${#out1}
  if (( actual_len > expected_len )); then
    echo "Round $round: CORRECTNESS FAIL — merged doc length $actual_len > expected $expected_len (duplication/corruption)" \
      | tee -a "$RESULTS"
    correct=0
  fi

  if (( converged == 1 && correct == 1 )); then
    echo "Round $round: Correctness: ok" | tee -a "$RESULTS"
  fi
}

# Run three rounds with different character sets.
run_round 1 "hello"    "world"    "!!!"
run_round 2 "abc"      "def"      "ghi"
run_round 3 "SyncText" "isgreat"  ":-)"

# ---------------------------------------------------------------------------
# run_far_apart: test local cursor anchor tracking during far-apart 
# concurrent typing.
# ---------------------------------------------------------------------------
run_far_apart() {
  local round="far-apart"
  
  # Pre-populate documents so they are identical.
  local base_text="line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\nline9\nline10\n"
  echo -ne "$base_text" > "$ROOT/hl_fa1_doc.txt"
  echo -ne "$base_text" > "$ROOT/hl_fa2_doc.txt"

  local FIFO1; FIFO1=$(mktemp -u /tmp/hl_fifo1_XXXX)
  local FIFO2; FIFO2=$(mktemp -u /tmp/hl_fifo2_XXXX)
  mkfifo "$FIFO1" "$FIFO2"

  "$BIN" hl_fa1 "$PORT1" --headless < "$FIFO1" > /tmp/hl_out1.txt 2>/tmp/hl_err1.txt &
  PID1=$!
  "$BIN" hl_fa2 "$PORT2" --headless < "$FIFO2" > /tmp/hl_out2.txt 2>/tmp/hl_err2.txt &
  PID2=$!

  sleep 1

  # A moves to end of "line1" (which is 5 chars).
  (
    echo "M 5"
    sleep 2
    for c in $(echo "AAA" | fold -w1); do echo "I $c"; sleep 0.1; done
    sleep 2
    echo "P"
  ) > "$FIFO1" &

  # B moves to end of "line10\n" (which is 61 chars, but let's just move 60 to be after line10).
  (
    echo "M 60"
    sleep 2
    for c in $(echo "BBB" | fold -w1); do echo "I $c"; sleep 0.1; done
    sleep 2
    echo "P"
  ) > "$FIFO2" &

  sleep 5

  local out1; out1=$(cat /tmp/hl_out1.txt 2>/dev/null || echo "")
  local out2; out2=$(cat /tmp/hl_out2.txt 2>/dev/null || echo "")

  kill "$PID1" "$PID2" 2>/dev/null || true
  wait "$PID1" "$PID2" 2>/dev/null || true
  rm -f "$FIFO1" "$FIFO2"
  rm -f "$ROOT/hl_fa1_doc.txt" "$ROOT/hl_fa2_doc.txt"

  if [[ "$out1" != "$out2" ]]; then
    echo "Round $round: CONVERGENCE FAIL" | tee -a "$RESULTS"
    echo "  u1: [$out1]" | tee -a "$RESULTS"
    echo "  u2: [$out2]" | tee -a "$RESULTS"
  else
    echo "Round $round: Converged: yes" | tee -a "$RESULTS"
  fi

  if grep -q "line1AAA" <<< "$out1" && grep -q "line10BBB" <<< "$out1"; then
    echo "Round $round: Correctness: ok" | tee -a "$RESULTS"
  else
    echo "Round $round: CORRECTNESS FAIL" | tee -a "$RESULTS"
    echo "Got: $out1" | tee -a "$RESULTS"
  fi
}

run_far_apart

echo ""
echo "=== Headless test complete. Results in headless_results.txt ===" | tee -a "$RESULTS"

# Exit non-zero if any failure was recorded.
if grep -q "FAIL" "$RESULTS"; then
  exit 1
fi
exit 0

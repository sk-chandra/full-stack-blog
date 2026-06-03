#!/bin/sh
# fuzz.sh — feed thousands of generated programs through the compiler and assert
# it never CRASHES. Only clean outcomes are acceptable: success (0), usage (64),
# compile error (65), runtime error (70), or a timeout (124). Anything else — a
# segfault (139), an abort, or an AddressSanitizer report (1/134) — is a bug, and
# the offending input is saved so it reproduces.
#
# Usage: tools/fuzz.sh [CNANO] [FUZZGEN] [ITERATIONS]
# Prefer the ASan build (build/cnano-debug) so memory errors are caught.
set -u

CNANO="${1:-./build/cnano-debug}"
GEN="${2:-./build/fuzzgen}"
N="${3:-2000}"

if [ ! -x "$CNANO" ] || [ ! -x "$GEN" ]; then
  echo "fuzz: need $CNANO and $GEN (run: make fuzz)"; exit 2
fi

OUT="$(mktemp -d)"
in="$OUT/in.cn"
crashes=0
i=0
while [ "$i" -lt "$N" ]; do
  i=$((i + 1))
  "$GEN" "$i" > "$in"
  # Exercise the non-executing compiler paths: each fully parses and type-checks
  # (and --emit-c also runs code generation), without running arbitrary programs —
  # so timeouts/OOM from execution don't muddy the signal.
  for mode in --emit-c --ir --asm; do
    timeout 10 "$CNANO" "$mode" "$in" >/dev/null 2>&1
    code=$?
    case "$code" in
      0 | 64 | 65 | 70 | 124) ;;
      *)
        crashes=$((crashes + 1))
        saved="$OUT/crash-seed${i}-${mode#--}-exit${code}.cn"
        cp "$in" "$saved"
        printf 'CRASH  seed=%-6s mode=%-8s exit=%-3s  saved=%s\n' "$i" "$mode" "$code" "$saved"
        ;;
    esac
  done
done

printf -- '----------------------------------------\n'
printf 'fuzz: %s seeds x 3 modes, %s crashes\n' "$N" "$crashes"
[ "$crashes" -eq 0 ]

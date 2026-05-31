#!/usr/bin/env bash
# run_tests.sh — a tiny end-to-end test harness for cnano.
#
# Each test feeds an expression to the cnano binary and checks the printed
# result (or, for error cases, the exit code). This is "golden" testing: we
# assert on observable behaviour, which is exactly what a language user cares
# about. Usage: ./run_tests.sh path/to/cnano
set -u

CNANO="${1:-./build/cnano}"
pass=0
fail=0
tmp="$(mktemp)"

# check NAME EXPRESSION EXPECTED  — expect successful run printing EXPECTED.
check() {
  local name="$1" expr="$2" expected="$3"
  printf '%s' "$expr" > "$tmp"
  local got
  got="$("$CNANO" "$tmp" 2>/dev/null)"
  if [ "$got" = "$expected" ]; then
    printf '  ok   %-22s %s = %s\n' "$name" "$expr" "$got"
    pass=$((pass + 1))
  else
    printf '  FAIL %-22s %s : expected %s, got %s\n' "$name" "$expr" "$expected" "$got"
    fail=$((fail + 1))
  fi
}

# check_err NAME EXPRESSION  — expect a non-zero exit code (an error).
check_err() {
  local name="$1" expr="$2"
  printf '%s' "$expr" > "$tmp"
  if "$CNANO" "$tmp" >/dev/null 2>&1; then
    printf '  FAIL %-22s %s : expected an error, but it succeeded\n' "$name" "$expr"
    fail=$((fail + 1))
  else
    printf '  ok   %-22s %s -> error (as expected)\n' "$name" "$expr"
    pass=$((pass + 1))
  fi
}

echo "Running cnano tests with: $CNANO"

check "addition"        "1 + 2"            "3"
check "precedence"      "1 + 2 * 3"        "7"
check "grouping"        "(1 + 2) * 3"      "9"
check "left-assoc-sub"  "10 - 2 - 3"       "5"
check "unary-negate"    "-5 + 8"           "3"
check "double-negate"   "--7"              "7"
check "integer-div"     "7 / 2"            "3"
check "big-expression"  "(1 + 2) * 3 - 10 / 2" "4"
check "nested-parens"   "((2))"            "2"

check_err "div-by-zero"   "1 / 0"
check_err "syntax-trail"  "1 +"
check_err "bad-char"      "1 $ 2"
check_err "unbalanced"    "(1 + 2"

rm -f "$tmp"
echo "-----------------------------------------"
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]

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

# --- helpers ---------------------------------------------------------------
# Run the given full PROGRAM source and compare its stdout to EXPECTED.
run_prog() { printf '%s' "$1" > "$tmp"; "$CNANO" "$tmp" 2>/dev/null; }

# check NAME EXPRESSION EXPECTED — wrap EXPRESSION as `print EXPRESSION;` and
# expect that single value printed. Most tests are about expressions, so this
# wrapper keeps them terse while exercising the real statement pipeline.
check() {
  local name="$1" expr="$2" expected="$3"
  local got
  got="$(run_prog "print $expr;")"
  if [ "$got" = "$expected" ]; then
    printf '  ok   %-22s %s = %s\n' "$name" "$expr" "$got"
    pass=$((pass + 1))
  else
    printf '  FAIL %-22s %s : expected %s, got %s\n' "$name" "$expr" "$expected" "$got"
    fail=$((fail + 1))
  fi
}

# check_prog NAME PROGRAM EXPECTED — run a full multi-line program (you write
# the `print`s and `;`s yourself) and compare its complete output. EXPECTED may
# contain newlines.
check_prog() {
  local name="$1" prog="$2" expected="$3"
  local got
  got="$(run_prog "$prog")"
  if [ "$got" = "$expected" ]; then
    printf '  ok   %-22s (program) ok\n' "$name"
    pass=$((pass + 1))
  else
    printf '  FAIL %-22s program output mismatch:\n--- expected ---\n%s\n--- got ---\n%s\n' \
      "$name" "$expected" "$got"
    fail=$((fail + 1))
  fi
}

# check_err NAME EXPRESSION — wrap as `print EXPRESSION;`, expect a non-zero exit
# (used for runtime/type errors inside an expression).
check_err() {
  local name="$1" expr="$2"
  printf 'print %s;' "$expr" > "$tmp"
  if "$CNANO" "$tmp" >/dev/null 2>&1; then
    printf '  FAIL %-22s %s : expected an error, but it succeeded\n' "$name" "$expr"
    fail=$((fail + 1))
  else
    printf '  ok   %-22s %s -> error (as expected)\n' "$name" "$expr"
    pass=$((pass + 1))
  fi
}

# check_prog_err NAME PROGRAM — run full PROGRAM source, expect a non-zero exit
# (used for statement-level syntax errors like a missing semicolon).
check_prog_err() {
  local name="$1" prog="$2"
  printf '%s' "$prog" > "$tmp"
  if "$CNANO" "$tmp" >/dev/null 2>&1; then
    printf '  FAIL %-22s : expected an error, but it succeeded\n' "$name"
    fail=$((fail + 1))
  else
    printf '  ok   %-22s -> error (as expected)\n' "$name"
    pass=$((pass + 1))
  fi
}

echo "Running cnano tests with: $CNANO"

# --- arithmetic (from the first slice) ---
check "addition"        "1 + 2"            "3"
check "precedence"      "1 + 2 * 3"        "7"
check "grouping"        "(1 + 2) * 3"      "9"
check "left-assoc-sub"  "10 - 2 - 3"       "5"
check "unary-negate"    "-5 + 8"           "3"
check "double-negate"   "--7"              "7"
check "integer-div"     "7 / 2"            "3"
check "big-expression"  "(1 + 2) * 3 - 10 / 2" "4"
check "nested-parens"   "((2))"            "2"

# --- literals (step 1) ---
check "true-literal"    "true"             "true"
check "false-literal"   "false"            "false"
check "nil-literal"     "nil"              "nil"

# --- logical not + truthiness (step 1) ---
check "not-true"        "!true"            "false"
check "not-false"       "!false"           "true"
check "not-nil"         "!nil"             "true"
check "not-zero-truthy" "!0"               "false"   # 0 is TRUTHY in cnano
check "double-not"      "!!false"          "false"

# --- comparisons (step 1) ---
check "less"            "1 < 2"            "true"
check "less-false"      "2 < 1"            "false"
check "less-equal"      "2 <= 2"           "true"
check "greater"         "5 > 3"            "true"
check "greater-equal"   "5 >= 5"           "true"

# --- equality, including cross-type (step 1) ---
check "int-equal"       "1 == 1"           "true"
check "int-not-equal"   "1 != 2"           "true"
check "bool-equal"      "true == true"     "true"
check "cross-type-neq"  "1 == true"        "false"   # no implicit coercion
check "nil-equal-nil"   "nil == nil"       "true"

# --- precedence across the new levels (step 1) ---
check "arith-vs-cmp"    "1 + 2 == 3"       "true"    # parses as (1+2)==3
check "cmp-vs-eq"       "1 < 2 == true"    "true"    # parses as (1<2)==true

# --- runtime type errors (step 1) ---
check_err "div-by-zero"     "1 / 0"
check_err "add-bool"        "1 + true"
check_err "negate-bool"     "-true"
check_err "compare-bool"    "true < false"
check_err "chained-compare" "1 < 2 < 3"            # (1<2)<3 -> true<3 -> error

# --- syntax / lexing errors ---
check_err "syntax-trail"  "1 +"
check_err "bad-char"      "1 @ 2"
check_err "unbalanced"    "(1 + 2"
check_err "lone-equals"   "1 = 2"                   # '=' alone is rejected
check_err "unknown-word"  "foo"                     # no variables yet

# --- statements & print (step 2) ---
# Multi-statement programs run top to bottom, each print on its own line.
check_prog "two-prints"     "print 1; print 2;"          "$(printf '1\n2')"
check_prog "print-order"    "print 10; print 20; print 30;" "$(printf '10\n20\n30')"
check_prog "expr-stmt-quiet" "1 + 2; print 99;"          "99"  # expr stmt is silent
check_prog "print-bool"     "print 3 < 5;"               "true"
check_prog "print-nil"      "print nil;"                 "nil"
check_prog "empty-program"  ""                            ""    # no statements, no output
# A runtime error mid-program: earlier prints still happen, then it aborts.
check_prog "partial-output" "print 1; print 2/0; print 3;" "1"
# Line comments are stripped by the lexer; division is NOT mistaken for one.
check_prog "line-comment"   "$(printf 'print 1; // ignored\nprint 2;')" "$(printf '1\n2')"
check_prog "div-not-comment" "print 8 / 4;"                "2"
check_prog "comment-only"   "// just a comment"            ""

# --- statement-level syntax errors (step 2) ---
check_prog_err "missing-semicolon" "print 1"
check_prog_err "expr-no-semicolon" "1 + 2"
check_prog_err "print-no-value"    "print ;"
check_prog_err "bare-semicolon"    ";"

# --- global variables (step 3) ---
check_prog "let-and-read"     "let x = 10; print x;"              "10"
check_prog "var-in-expr"      "let x = 3; print x * x + 1;"       "10"
check_prog "reassign"         "let x = 1; x = 99; print x;"       "99"
check_prog "assign-chain"     "let a=0; let b=0; a = b = 5; print a; print b;" "$(printf '5\n5')"
check_prog "assign-is-expr"   "let a = 0; print a = 7;"           "7"
check_prog "var-keeps-state"  "let n = 1; n = n + n; n = n + n; print n;" "4"
# Force the hash table to grow past its initial 8 buckets (>6 keys at 0.75 load).
check_prog "many-globals"     "let a=1;let b=2;let c=3;let d=4;let e=5;let f=6;let g=7;let h=8;let i=9;print a+b+c+d+e+f+g+h+i;" "45"

# --- strings (step 3) ---
check_prog "string-literal"   'print "hello";'                    "hello"
check_prog "string-concat"    'print "foo" + "bar";'              "foobar"
check_prog "string-in-var"    'let s = "hi"; print s + s;'        "hihi"
check_prog "string-eq-intern" 'print "ab" == "a" + "b";'         "true"
check_prog "string-neq"       'print "a" == "b";'                 "false"
check_prog "string-int-neq"   'print "1" == 1;'                   "false"

# --- variable & string runtime/type errors (step 3) ---
check_prog_err "undefined-read"   "print y;"
check_prog_err "undefined-assign" "z = 5;"
check_prog_err "bad-lvalue"       "1 + 2 = 3;"
check_prog_err "let-no-init"      "let x;"
check_prog_err "let-no-name"      "let = 5;"
check_prog_err "string-plus-int"  'print "a" + 1;'
check_prog_err "unterminated-str" 'print "oops;'

# --- local variables & block scope (step 4) ---
check_prog "block-local"      "{ let a = 5; print a; }"           "5"
check_prog "two-locals"       "{ let a = 3; let b = 4; print a + b; }" "7"
check_prog "local-sees-global" "let g = 100; { let a = 1; print a + g; }" "101"
check_prog "local-assign"     "{ let c = 1; c = c + 9; print c; }" "10"
# Shadowing: each scope has its own x; exiting restores the outer one.
check_prog "shadowing" \
  "let x = 1; print x; { let x = 2; print x; { let x = 3; print x; } print x; } print x;" \
  "$(printf '1\n2\n3\n2\n1')"
check_prog "shadow-global-in-block" "let n = 9; { let n = 1; print n; } print n;" "$(printf '1\n9')"
check_prog "nested-blocks"    "{ let a = 1; { let b = 2; { print a + b; } } }" "3"
# A global declared after a block still works (block locals were cleaned up).
check_prog "stack-balanced"   "{ let a = 1; } let g = 7; print g;" "7"

# --- local/scope compile & runtime errors (step 4) ---
check_prog_err "local-escapes-scope" "{ let s = 1; } print s;"   # s is gone -> undefined
check_prog_err "redeclare-local"     "{ let d = 1; let d = 2; }"
check_prog_err "self-init"           "{ let e = e; }"
check_prog_err "unclosed-block"      "{ let a = 1;"
check_prog_err "unexpected-rbrace"   "let a = 1; }"
# Regression guards: tokens that cannot start an expression once caused the
# parser's recovery loop to spin forever. These must terminate (not hang).
check_prog_err "stray-rbrace"        "}"
check_prog_err "many-rbrace"         "} } }"
check_prog_err "leading-operator"    "* 3;"

# --- control flow (step 5) ---
# if / else
check_prog "if-true"      'if (1 < 2) print "y"; else print "n";'   "y"
check_prog "if-false"     'if (1 > 2) print "y"; else print "n";'   "n"
check_prog "if-no-else"   'if (false) print "x"; print "after";'    "after"
check_prog "if-block"     'if (true) { let a = 5; print a; }'       "5"
# while
check_prog "while-count"  'let i=0; while (i<3) { print i; i=i+1; }' "$(printf '0\n1\n2')"
check_prog "while-never"  'while (false) print "x"; print "ok";'    "ok"
# for (desugars to block + while)
check_prog "for-count"    'for (let i=0; i<3; i=i+1) print i;'      "$(printf '0\n1\n2')"
check_prog "for-sum"      'let s=0; for (let i=1; i<=5; i=i+1) s=s+i; print s;' "15"
check_prog "for-scope"    'for (let i=0; i<1; i=i+1) {} print "loopvar-gone";' "loopvar-gone"
# and / or short-circuit: result is the deciding operand
check_prog "and-true"     'print true and 5;'                       "5"
check_prog "and-false"    'print false and 5;'                      "false"
check_prog "or-first"     'print 7 or 9;'                           "7"
check_prog "or-second"    'print nil or 9;'                         "9"
check_prog "or-fallback"  'print nil or "default";'                 "default"
# Short-circuit must NOT evaluate the skipped side (no side effect).
check_prog "and-shortcct" 'let x=0; false and (x=99); print x;'     "0"
check_prog "or-shortcct"  'let x=0; true or (x=99); print x;'       "0"
# Nested control flow: count multiples of 3 below 10.
check_prog "nested-flow"  'let c=0; for (let n=1; n<10; n=n+1) { if (n-(n/3)*3==0) c=c+1; } print c;' "3"
# A loop that runs long enough to expose any stack imbalance (cap is 256).
check_prog "loop-balance" 'let i=0; while (i<500) i=i+1; print i;'  "500"

# --- control-flow syntax errors (step 5) ---
check_prog_err "if-no-paren"    'if true print 1;'
check_prog_err "while-no-paren" 'while true print 1;'
check_prog_err "for-no-parts"   'for print 1;'
check_prog_err "dangling-else"  'else print 1;'

rm -f "$tmp"
echo "-----------------------------------------"
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]

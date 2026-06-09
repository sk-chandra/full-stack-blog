#!/usr/bin/env bash
# run_tests.sh — a tiny end-to-end test harness for cnano.
#
# Each test feeds an expression to the cnano binary and checks the printed
# result (or, for error cases, the exit code). This is "golden" testing: we
# assert on observable behaviour, which is exactly what a language user cares
# about. Usage: ./run_tests.sh path/to/cnano
set -u

CNANO="${1:-./build/cnano}"
# Resolve to an absolute path: the module tests `cd` into a temp dir to run the
# entry file, after which a relative ./build/cnano would no longer resolve.
case "$CNANO" in
  /*) ;; # already absolute
  *) CNANO="$(cd "$(dirname "$CNANO")" && pwd)/$(basename "$CNANO")" ;;
esac
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

# check_diag NAME PROGRAM SUBSTRING — run PROGRAM (expecting failure) and assert
# its stderr (diagnostics) CONTAINS the given substring. Used to pin the shape of
# error messages, e.g. a caret-underline line.
check_diag() {
  local name="$1" prog="$2" needle="$3"
  printf '%s' "$prog" > "$tmp"
  local err; err="$("$CNANO" "$tmp" 2>&1 >/dev/null)"
  case "$err" in
    *"$needle"*)
      printf '  ok   %-22s diagnostic ok\n' "$name"; pass=$((pass + 1)) ;;
    *)
      printf '  FAIL %-22s diagnostic missing [%s] in:\n%s\n' "$name" "$needle" "$err"
      fail=$((fail + 1)) ;;
  esac
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

# check_clean_error NAME PROGRAM — expect a CLEAN compile error (exit 65), not a
# crash. Unlike check_prog_err (any non-zero), this fails on a segfault/abort/ASan
# exit, so it is the right tool for parser-robustness regressions.
check_clean_error() {
  local name="$1" prog="$2"
  printf '%s' "$prog" > "$tmp"
  "$CNANO" "$tmp" >/dev/null 2>&1
  local code=$?
  if [ "$code" -eq 65 ]; then
    printf '  ok   %-22s -> clean compile error\n' "$name"; pass=$((pass + 1))
  else
    printf '  FAIL %-22s : expected clean error (65), got exit %s\n' "$name" "$code"; fail=$((fail + 1))
  fi
}

# check_dbg NAME PROGRAM COMMANDS PATTERN — run PROGRAM under the stepping
# debugger (--debug) feeding COMMANDS (\n-separated) on stdin, and assert the
# combined session output matches the shell glob PATTERN (use * between the
# substrings that must appear in order).
check_dbg() {
  local name="$1" prog="$2" cmds="$3" pattern="$4"
  printf '%s' "$prog" > "$tmp"
  local out; out="$(printf '%b' "$cmds" | "$CNANO" --debug "$tmp" 2>&1)"
  case "$out" in
    $pattern)
      printf '  ok   %-22s (debugger) ok\n' "$name"; pass=$((pass + 1)) ;;
    *)
      printf '  FAIL %-22s debugger output mismatch:\n%s\n' "$name" "$out"; fail=$((fail + 1)) ;;
  esac
}

# check_native NAME PROGRAM EXPECTED — compile PROGRAM to a NATIVE binary via the
# C backend, run it, and compare output. Proves the --native path produces a real
# executable whose behaviour matches the VM. Skipped if no C compiler is found.
HAVE_CC=0
command -v cc >/dev/null 2>&1 && HAVE_CC=1
check_native() {
  local name="$1" prog="$2" expected="$3"
  if [ "$HAVE_CC" -eq 0 ]; then
    printf '  skip %-22s (no cc found)\n' "$name"; return
  fi
  printf '%s' "$prog" > "$tmp"
  local bin="${tmp}.native"
  if ! "$CNANO" --native "$tmp" -o "$bin" >/dev/null 2>&1; then
    printf '  FAIL %-22s : native compile failed\n' "$name"
    fail=$((fail + 1)); return
  fi
  local got; got="$("$bin" 2>/dev/null)"
  rm -f "$bin"
  if [ "$got" = "$expected" ]; then
    printf '  ok   %-22s (native) ok\n' "$name"
    pass=$((pass + 1))
  else
    printf '  FAIL %-22s native: expected [%s] got [%s]\n' "$name" "$expected" "$got"
    fail=$((fail + 1))
  fi
}

# check_native_err NAME PROGRAM — expect the --native backend to REJECT a program
# (out-of-subset feature). The VM may still accept it; only native is checked.
check_native_err() {
  local name="$1" prog="$2"
  if [ "$HAVE_CC" -eq 0 ]; then
    printf '  skip %-22s (no cc found)\n' "$name"; return
  fi
  printf '%s' "$prog" > "$tmp"
  if "$CNANO" --native "$tmp" -o "${tmp}.native" >/dev/null 2>&1; then
    rm -f "${tmp}.native"
    printf '  FAIL %-22s : native expected rejection, but it compiled\n' "$name"
    fail=$((fail + 1))
  else
    printf '  ok   %-22s -> native rejected (as expected)\n' "$name"
    pass=$((pass + 1))
  fi
}

# check_asm NAME PROGRAM EXPECTED — emit x86-64 assembly (--asm), assemble + link
# it with cc, run the binary, and compare its stdout. This proves the machine-code
# backend agrees with the VM down to real instructions.
check_asm() {
  local name="$1" prog="$2" expected="$3"
  if [ "$HAVE_CC" -eq 0 ]; then
    printf '  skip %-22s (no cc found)\n' "$name"; return
  fi
  printf '%s' "$prog" > "$tmp"
  local asm="${tmp}.s" bin="${tmp}.asmbin"
  if ! "$CNANO" --asm "$tmp" > "$asm" 2>/dev/null; then
    printf '  FAIL %-22s : --asm emission failed\n' "$name"; fail=$((fail + 1)); return
  fi
  if ! cc "$asm" -o "$bin" >/dev/null 2>&1; then
    printf '  FAIL %-22s : assembling emitted code failed\n' "$name"; fail=$((fail + 1)); rm -f "$asm"; return
  fi
  local got; got="$("$bin" 2>/dev/null)"
  rm -f "$asm" "$bin"
  if [ "$got" = "$expected" ]; then
    printf '  ok   %-22s (asm) ok\n' "$name"; pass=$((pass + 1))
  else
    printf '  FAIL %-22s asm: expected [%s] got [%s]\n' "$name" "$expected" "$got"; fail=$((fail + 1))
  fi
}

# check_asm_err NAME PROGRAM — expect the x86-64 backend to REJECT a program
# (out-of-subset: bool/float/control-flow/calls).
check_asm_err() {
  local name="$1" prog="$2"
  printf '%s' "$prog" > "$tmp"
  if "$CNANO" --asm "$tmp" >/dev/null 2>&1; then
    printf '  FAIL %-22s : asm expected rejection, but it emitted\n' "$name"; fail=$((fail + 1))
  else
    printf '  ok   %-22s -> asm rejected (as expected)\n' "$name"; pass=$((pass + 1))
  fi
}

# check_elf NAME PROGRAM EXPECTED — compile PROGRAM straight to an ELF
# executable (--elf: cnano as its own assembler and linker, no cc at all), run
# it, and compare output. No toolchain needed, so never skipped.
check_elf() {
  local name="$1" prog="$2" expected="$3"
  printf '%s' "$prog" > "$tmp"
  local bin="${tmp}.elf"
  if ! "$CNANO" --elf "$tmp" -o "$bin" >/dev/null 2>&1; then
    printf '  FAIL %-22s : --elf emission failed\n' "$name"; fail=$((fail + 1)); return
  fi
  local got; got="$("$bin" 2>/dev/null)"
  local code=$?
  rm -f "$bin"
  if [ "$got" = "$expected" ] && [ "$code" -eq 0 ]; then
    printf '  ok   %-22s (elf) ok\n' "$name"; pass=$((pass + 1))
  else
    printf '  FAIL %-22s elf: expected [%s] got [%s] (exit %s)\n' "$name" "$expected" "$got" "$code"; fail=$((fail + 1))
  fi
}

# check_elf_err NAME PROGRAM — expect the ELF backend to reject the program.
check_elf_err() {
  local name="$1" prog="$2"
  printf '%s' "$prog" > "$tmp"
  if "$CNANO" --elf "$tmp" -o "${tmp}.elf" >/dev/null 2>&1; then
    rm -f "${tmp}.elf"
    printf '  FAIL %-22s : elf expected rejection, but it emitted\n' "$name"; fail=$((fail + 1))
  else
    printf '  ok   %-22s -> elf rejected (as expected)\n' "$name"; pass=$((pass + 1))
  fi
}

# check_module NAME ENTRY_REL EXPECTED FILE1 BODY1 [FILE2 BODY2 ...] — write a set
# of files into a fresh temp directory (relative paths preserved, subdirs created)
# and run ENTRY_REL on the VM, comparing stdout. Exercises `import` resolution.
check_module() {
  local name="$1" entry="$2" expected="$3"; shift 3
  local dir; dir="$(mktemp -d)"
  while [ "$#" -ge 2 ]; do
    local rel="$1" body="$2"; shift 2
    mkdir -p "$dir/$(dirname "$rel")"
    printf '%s' "$body" > "$dir/$rel"
  done
  local got; got="$(cd "$dir" && "$CNANO" "$entry" 2>/dev/null)"
  rm -rf "$dir"
  if [ "$got" = "$expected" ]; then
    printf '  ok   %-22s (module) ok\n' "$name"
    pass=$((pass + 1))
  else
    printf '  FAIL %-22s module: expected [%s] got [%s]\n' "$name" "$expected" "$got"
    fail=$((fail + 1))
  fi
}

# check_module_err NAME ENTRY_REL FILE1 BODY1 [...] — like check_module but expects
# a non-zero exit (a bad import: missing file, misplaced import, …).
check_module_err() {
  local name="$1" entry="$2"; shift 2
  local dir; dir="$(mktemp -d)"
  while [ "$#" -ge 2 ]; do
    local rel="$1" body="$2"; shift 2
    mkdir -p "$dir/$(dirname "$rel")"
    printf '%s' "$body" > "$dir/$rel"
  done
  if (cd "$dir" && "$CNANO" "$entry") >/dev/null 2>&1; then
    rm -rf "$dir"
    printf '  FAIL %-22s : expected an import error, but it succeeded\n' "$name"
    fail=$((fail + 1))
  else
    rm -rf "$dir"
    printf '  ok   %-22s -> error (as expected)\n' "$name"
    pass=$((pass + 1))
  fi
}

# check_module_diag NAME ENTRY SUBSTRING FILE1 BODY1 [...] — like check_module but
# asserts the program's stderr (diagnostics) contains SUBSTRING. Used to check
# that a multi-file error names the file it came from.
check_module_diag() {
  local name="$1" entry="$2" needle="$3"; shift 3
  local dir; dir="$(mktemp -d)"
  while [ "$#" -ge 2 ]; do
    local rel="$1" body="$2"; shift 2
    mkdir -p "$dir/$(dirname "$rel")"
    printf '%s' "$body" > "$dir/$rel"
  done
  local err; err="$(cd "$dir" && "$CNANO" "$entry" 2>&1 >/dev/null)"
  rm -rf "$dir"
  case "$err" in
    *"$needle"*) printf '  ok   %-22s diagnostic ok\n' "$name"; pass=$((pass + 1)) ;;
    *) printf '  FAIL %-22s missing [%s] in:\n%s\n' "$name" "$needle" "$err"; fail=$((fail + 1)) ;;
  esac
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
check "modulo"          "17 % 5"           "2"
check "float-lit"       "3.14"             "3.14"
check "float-add"       "1.5 + 2.5"        "4.0"
check "float-div"       "10.0 / 4.0"       "2.5"
check "float-neg"       "-1.5"             "-1.5"
check "float-whole"     "6.0 / 2.0"        "3.0"
check "float-promote"   "5 + 2.5"          "7.5"
check "float-int-div"   "10 / 4"           "2"
# Numeric literal forms (step 48): hex/binary/octal prefixes + `_` separators.
check "lit-hex"         "0xFF"             "255"
check "lit-hex-upper"   "0XdeadBEEF"       "3735928559"
check "lit-bin"         "0b1010"           "10"
check "lit-oct"         "0o17"             "15"
check "lit-sep-dec"     "1_000_000"        "1000000"
check "lit-sep-hex"     "0xff_ff"          "65535"
check "lit-sep-float"   "1_000.5"          "1000.5"
check "lit-leading-zero" "017"             "17"
check "lit-mixed-bases" "0x10 + 0b10 + 0o10" "26"
check "lit-hex-eq"      "0xFF == 255"      "true"
check_prog "lit-range-ok" 'for (let i in 0..3) print i;' "$(printf '0\n1\n2')"
check_prog_err "lit-bad-hex"  'print 0x;'
check_prog_err "lit-bad-bin"  'print 0b2;'
check_native "nat-lit-hex" 'fn f(): int { return 0xFF + 0b1; } print f();' "256"
check "float-mixed-div" "10.0 / 4"         "2.5"
check "float-cmp"       "1.5 < 2.0"        "true"
check "float-eq-int"    "3.0 == 3"         "true"
# Regression: hashValue must agree with valuesEqual (1 == 1.0), so int and
# integral-float map keys are the SAME key.
check_prog "float-key-int" 'let m = {}; m[1] = "a"; print m[1.0]; print m.has(1.0);' "$(printf 'a\ntrue')"
check_prog "float-key-dedup" 'let m = {}; m[1.0] = "f"; m[1] = "i"; print m.len();' "1"
check_prog "float-key-frac"  'let m = {}; m[2.5] = "x"; print m[2.5]; print m.has(3.5);' "$(printf 'x\nfalse')"
check "float-nan"       "0.0 / 0.0"        "nan"
check "float-inf"       "1.0 / 0.0"        "inf"
check "float-neg-inf"   "-1.0 / 0.0"       "-inf"
check "float-type"      'type(3.14)'       "float"
check_prog "float-str"  'print "pi=" + str(3.14);' "pi=3.14"
check_prog "float-typed-fn" 'fn avg(a: float, b: float): float { return (a+b)/2.0; } print avg(1.0, 4.0);' "2.5"
check_prog_err "float-mod-err" 'print 3.5 % 2;'
# float is a scalar, so the native backend now supports it (step 39). Arithmetic,
# comparisons, division (IEEE, by-zero -> inf/nan) and the numeric built-ins all
# lower to C, matching the VM byte-for-byte.
check_native "nat-float-id"   'fn f(x: float): float { return x; } print f(1.5);' "1.5"
check_native "nat-float-arith" 'fn h(a: float, b: float): float { return sqrt(a*a + b*b); } print h(3.0, 4.0);' "5.0"
check_native "nat-float-div"  'fn d(a: float, b: float): float { return a / b; } print d(7.0, 2.0);' "3.5"
check_native "nat-float-divzero" 'fn d(a: float, b: float): float { return a / b; } print d(1.0, 0.0);' "inf"
check_native "nat-float-cmp"  'fn lt(a: float, b: float): bool { return a < b; } print lt(2.0, 3.0);' "true"
check_native "nat-float-whole" 'fn f(x: float): float { return x * 2.0; } print f(3.0);' "6.0"
check_native "nat-float-math" 'print pow(2.0, 10.0);' "1024.0"
check_native "nat-float-neg"  'fn f(x: float): float { return -x; } print f(3.5);' "-3.5"
check_native "nat-abs-int"    'print abs(-7);' "7"
check_native "nat-abs-float"  'print abs(-3.5);' "3.5"
check_native "nat-min-float"  'print min(2.5, 1.5);' "1.5"
check_native "nat-max-int"    'print max(4, 9);' "9"
check_native "nat-float-global" 'let pi: float = 3.0; fn area(r: float): float { return pi * r * r; } print area(2.0);' "12.0"
check "bit-and"         "12 & 10"          "8"
check "bit-or"          "12 | 10"          "14"
check "bit-xor"        "12 ^ 10"          "6"
check "bit-not"         "~5"               "-6"
check "bit-shl"         "1 << 4"           "16"
check "bit-shr"         "256 >> 2"         "64"
check "bit-prec"        "1 | 2 & 3"        "3"
check "bit-shift-prec"  "1 << 2 + 1"       "8"
check "bit-fold"        "(255 & 15) | 16"  "31"
check_prog_err "bit-type-err"   'print 5 & true;'
check_prog_err "bit-shift-oob"  'print 1 << 70;'
check_native "nat-bitwise" 'fn f(a: int, b: int): int { return (a & b) | (a << 1) ^ ~b; } print f(12, 10);' "-19"
check "modulo-prec"     "1 + 8 % 3"        "3"
check "modulo-even"     "10 % 2"           "0"
check "big-expression"  "(1 + 2) * 3 - 10 / 2" "4"
check "nested-parens"   "((2))"            "2"
check_prog_err "modulo-by-zero" 'print 5 % 0;'
check_native "nat-modulo" 'fn r(a: int, b: int): int { return a % b; } print r(17, 5);' "2"

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
# Parser robustness (step 74, found by the fuzzer): a token that can't start an
# expression returns NULL up the chain; a following '=' / compound-assign / match
# subject must NOT be dereferenced as a node. These must error cleanly, not crash.
check_clean_error "asn-null-lhs"   "* = 1;"
check_clean_error "asn-null-brace" "} = 1;"
check_clean_error "asn-null-cmpd"  "+ += 2;"
check_clean_error "match-null-subj" "match (*) { _ => print 1; }"

# --- spec conformance (step 76) ---
# Corners pinned by docs/SPEC.md. `1.` is NOT a float literal (the dot is a
# member access, so this is a clean parse error)...
check_clean_error "spec-dot-not-float" 'print 1.;'
# ...and `?:` is right-associative: a ? b : c ? d : e == a ? b : (c ? d : e).
check_prog "spec-ternary-rassoc" 'print false ? "a" : true ? "b" : "c";' "b"

# --- the stepping debugger (step 77) ---
dbg_prog='let total = 0;
fn addTo(n) {
  let doubled = n * 2;
  total = total + doubled;
  return total;
}
addTo(5);
print total;'
# Pauses on the first line; with no commands (EOF) it continues to completion.
check_dbg "dbg-first-stop" "$dbg_prog" "" '*stopped at line 1*10*'
# A breakpoint inside a function; print a parameter, a local (via the compiler's
# debug-info table — names the bytecode itself discarded), and a global.
check_dbg "dbg-break-print" "$dbg_prog" 'b 4\nc\np n\np doubled\np total\nc\n' \
  '*breakpoint at line 4*n = 5*doubled = 10*total = 0*10*'
# The backtrace shows the paused frame and its caller.
check_dbg "dbg-backtrace" "$dbg_prog" 'b 3\nc\nbt\nc\n' \
  '*#0 addTo*#1 <script>*'
# vars lists every local in scope at the pause.
check_dbg "dbg-vars" "$dbg_prog" 'b 4\nc\nvars\nc\n' '*n = 5*doubled = 10*'
dbg_step='fn twice(x) {
  return x * 2;
}
let a = twice(4);
let b = a + 1;
print b;'
# `n` steps OVER the call: 1 -> 4 -> 5 (no re-pause at 4 when the callee
# returns), and `a` is then assigned.
check_dbg "dbg-step-over" "$dbg_step" 'n\nn\nn\np a\nc\n' \
  '*stopped at line 1*stopped at line 4*stopped at line 5*a = 8*'
# `s` steps INTO the call: line 4 -> line 2, where the parameter is in scope.
check_dbg "dbg-step-into" "$dbg_step" 's\ns\np x\nc\n' \
  '*stopped at line 4*stopped at line 2*x = 4*'
# A breakpoint in a loop re-fires on every iteration.
check_dbg "dbg-loop-bp" 'let i = 0;
while (i < 2) {
  i = i + 1;
}
print i;' 'b 3\nc\np i\nc\np i\nc\n' '*i = 0*i = 1*2*'
# An unknown name reports cleanly instead of crashing.
check_dbg "dbg-unknown-var" "$dbg_prog" 'p zzz\nc\n' "*no variable named 'zzz'*"

# --- global variables (step 3) ---
check_prog "let-and-read"     "let x = 10; print x;"              "10"
check_prog "var-in-expr"      "let x = 3; print x * x + 1;"       "10"
check_prog "reassign"         "let x = 1; x = 99; print x;"       "99"
check_prog "assign-chain"     "let a=0; let b=0; a = b = 5; print a; print b;" "$(printf '5\n5')"
check_prog "assign-is-expr"   "let a = 0; print a = 7;"           "7"
check_prog "var-keeps-state"  "let n = 1; n = n + n; n = n + n; print n;" "4"
# Force the hash table to grow past its initial 8 buckets (>6 keys at 0.75 load).
check_prog "many-globals"     "let a=1;let b=2;let c=3;let d=4;let e=5;let f=6;let g=7;let h=8;let i=9;print a+b+c+d+e+f+g+h+i;" "45"
# Regression: with exactly 6 globals (count at the 0.75 load of an 8-slot table),
# ASSIGNING to a global inside a loop used to trigger a table resize on the very
# first update — which rehashed every entry to a new index WITHOUT bumping the
# inline-cache generation, so later reads hit an empty bucket and saw nil. The fix
# is that an update never grows the table. acc must accumulate 4*(1+2+3+4)=40.
check_prog "global-cache-resize" "let a=1;let b=2;let c=3;let d=4;let acc=0;let i=0; while(i<4){ acc=acc+a+b+c+d; i=i+1; } print acc;" "40"

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
# do/while: the body runs at least once, even when the condition starts false.
check_prog "dowhile-count" 'let i=0; do { print i; i+=1; } while (i<3);' "$(printf '0\n1\n2')"
check_prog "dowhile-once"  'do print "once"; while (false);'          "once"
check_prog "dowhile-sum"   'let n=5; let s=0; do { s+=n; n-=1; } while (n>0); print s;' "15"
check_prog "dowhile-break" 'let k=0; do { k+=1; if (k==3) break; print k; } while (k<10); print "end";' "$(printf '1\n2\nend')"
check_prog "dowhile-cont"  'let k=0; do { k+=1; if (k==2) continue; print k; } while (k<4);' "$(printf '1\n3\n4')"
check_native "nat-dowhile" 'fn cd(n: int): int { let s=0; do { s+=n; n-=1; } while (n>0); return s; } print cd(5);' "15"
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

# --- conditional (ternary) expression `c ? a : b` (step 42) ---
check "cond-true"      '5 > 3 ? "big" : "small"' "big"
check "cond-false"     '1 > 3 ? "big" : "small"' "small"
check "cond-arith"     '(true ? 10 : 20) + 1'    "11"
check "cond-prec"      'true ? 1 : 0 + 5'        "1"   # `:` binds looser than `+`
check_prog "cond-nested" 'fn s(n){return n>0 ? "pos" : n<0 ? "neg" : "zero";} print s(5); print s(-2); print s(0);' "$(printf 'pos\nneg\nzero')"
check_prog "cond-in-let" 'let n=7; let p = n%2==0 ? "even" : "odd"; print p;' "odd"
check_prog "cond-assign-branch" 'let x=0; let y = true ? (x=1) : (x=2); print x; print y;' "$(printf '1\n1')"
# Only the taken branch runs (the other must have no effect).
check_prog "cond-shortcct" 'let x=0; false ? (x=1) : (x=2); print x;' "2"
# Native: a ternary with same-typed scalar branches lowers to C's `?:`.
check_native "nat-cond"     'fn c(n: int): str { return n > 0 ? "pos" : "nonpos"; } print c(7); print c(-1);' "$(printf 'pos\nnonpos')"
check_native "nat-cond-int" 'fn a(n: int): int { return n < 0 ? -n : n; } print a(-9);' "9"
# Native rejects a ternary whose branches differ in type.
check_native_err "nat-cond-mismatch" 'fn f(b: bool): int { return b ? 1 : 0 > 1; } print f(true);'
# Nested control flow: count multiples of 3 below 10.
check_prog "nested-flow"  'let c=0; for (let n=1; n<10; n=n+1) { if (n-(n/3)*3==0) c=c+1; } print c;' "3"
# A loop that runs long enough to expose any stack imbalance (cap is 256).
check_prog "loop-balance" 'let i=0; while (i<500) i=i+1; print i;'  "500"

# --- control-flow syntax errors (step 5) ---
check_prog_err "if-no-paren"    'if true print 1;'
check_prog_err "while-no-paren" 'while true print 1;'
check_prog_err "for-no-parts"   'for print 1;'
check_prog_err "dangling-else"  'else print 1;'

# --- functions (step 6) ---
check_prog "fn-basic"     'fn add(a,b){return a+b;} print add(3,4);'           "7"
check_prog "fn-no-args"   'fn answer(){return 42;} print answer();'            "42"
check_prog "fn-void-nil"  'fn p(x){print x;} print p(5);'                      "$(printf '5\nnil')"
check_prog "fn-implicit-nil" 'fn f(){} print f();'                            "nil"
check_prog "fn-locals"    'fn poly(x){let s=x*x; return s+x+1;} print poly(5);' "31"
check_prog "fn-recursion" 'fn fact(n){if(n<=1)return 1; return n*fact(n-1);} print fact(5);' "120"
check_prog "fn-fib"       'fn fib(n){if(n<2)return n; return fib(n-1)+fib(n-2);} print fib(10);' "55"
check_prog "fn-mutual"    'fn ev(n){if(n==0)return true; return od(n-1);} fn od(n){if(n==0)return false; return ev(n-1);} print ev(8);' "true"

# --- tail-call optimisation (step 58) ---
# Deep tail recursion runs in O(1) stack (the frame cap is only 64), so these
# would overflow WITHOUT tco; with it they return normally.
check_prog "tco-deep"     'fn c(n, acc) { if (n == 0) { return acc; } return c(n-1, acc+1); } print c(100000, 0);' "100000"
check_prog "tco-mutual"   'fn ev(n){ if(n==0){return true;} return od(n-1); } fn od(n){ if(n==0){return false;} return ev(n-1); } print ev(100000);' "true"
check_prog "tco-value"    'fn id(x){ return x; } fn w(x){ return id(x); } print w(42);' "42"
# A tail call inside a `try` must NOT be optimised away — the catch must still see
# a throw from the callee.
check_prog "tco-try-catch" 'fn boom(){ throw "x"; } fn f(){ try { return boom(); } catch (e) { return "c:"+e; } } print f();' "c:x"
# Non-tail recursion is unaffected: it still overflows safely (controlled error).
check_prog_err "tco-nontail-of" 'fn f(n) { return n * f(n-1); } print f(1000000);'

# --- generators / yield (step 59) ---
check_prog "gen-basic"    'fn c(n){let i=0; while(i<n){yield i; i+=1;}} let g=c(3); print g.next(); print g.next(); print g.next();' "$(printf '0\n1\n2')"
check_prog "gen-done"     'fn c(){ yield 1; } let g=c(); print g.done(); g.next(); g.next(); print g.done();' "$(printf 'false\ntrue')"
check_prog "gen-loop"     'fn c(n){let i=0; while(i<n){yield i*i; i+=1;}} let g=c(5); let s=0; while(!g.done()){ let v=g.next(); if(g.done()){break;} s+=v; } print s;' "30"
check_prog "gen-state"    'fn fib(){ let a=0; let b=1; while(true){ yield a; let t=a+b; a=b; b=t; } } let g=fib(); let out=[]; let i=0; while(i<8){ out.push(g.next()); i+=1; } print out.join(",");' "0,1,1,2,3,5,8,13"
check_prog "gen-interleave" 'fn c(n){let i=0; while(i<n){yield i; i+=1;}} let a=c(9); let b=c(9); print a.next(); print b.next(); print a.next(); print b.next();' "$(printf '0\n0\n1\n1')"
check_prog "gen-type"     'fn c(){ yield 1; } print type(c());' "generator"
check_prog "gen-heap"     'fn w(n){let i=0; while(i<n){yield "w"+str(i); i+=1;}} let g=w(3); let o=[]; while(!g.done()){ let x=g.next(); if(g.done()){break;} o.push(x); } print o.join(",");' "w0,w1,w2"
check_prog_err "gen-yield-toplevel" 'yield 5;'
check_native_err "nat-rej-gen" 'fn g(): int { yield 1; return 0; } print 1;'
check_prog "fn-nested"    'fn outer(){fn inner(x){return x*2;} return inner(21);} print outer();' "42"
check_prog "fn-as-value"  'fn sq(x){return x*x;} let f = sq; print f(9);'      "81"
check_prog "fn-early-ret" 'fn f(x){if(x>0)return "pos"; return "nonpos";} print f(5); print f(-1);' "$(printf 'pos\nnonpos')"
check_prog "fn-bare-ret"  'fn f(){return;} print f();'                        "nil"
check_prog "fn-uses-global" 'let g=100; fn add(x){return x+g;} print add(1);'  "101"
check_prog "fn-arg-expr"  'fn id(x){return x;} print id(2+3*4);'               "14"

# --- anonymous function expressions / lambdas (step 43) ---
check_prog "lam-arrow"    'let d = fn(x) => x*2; print d(21);'                 "42"
check_prog "lam-block"    'let a = fn(x,y){return x+y;}; print a(3,4);'        "7"
check_prog "lam-inline"   'print (fn(n) => n+1)(41);'                          "42"
check_prog "lam-hof"      'print [1,2,3,4].map(fn(x) => x*x).filter(fn(x)=>x>4).reduce(fn(a,b)=>a+b, 0);' "25"
check_prog "lam-closure"  'fn adder(n){return fn(x) => x+n;} let a=adder(10); print a(5);' "15"
check_prog "lam-noargs"   'let f = fn() => 7; print f();'                      "7"
check_prog "lam-shared"   'fn mk(){let c=0; let inc=fn()=>c; return inc;} print mk()();' "0"
check_prog "lam-typed"    'let id = fn(x: int): int => x; print id(99);'       "99"
check_prog_err "lam-typed-arg-err" 'let f = fn(a: int): int => a; print f("x");'
check_prog_err "lam-typed-ret-err" 'let f = fn(): int => "no"; print f();'
check_native_err "nat-rej-lambda" 'fn run(): int { let f = fn(x: int): int => x; return f(3); } print run();'

# --- function errors (step 6) ---
check_prog_err "fn-too-few-args"  'fn f(a,b){return a;} f(1);'
check_prog_err "fn-too-many-args" 'fn f(a){return a;} f(1,2);'
check_prog_err "call-non-fn"      'let x=5; x();'
check_prog_err "call-int"         '(3)();'
check_prog_err "return-top-level" 'return 5;'
check_prog_err "fn-no-name"       'fn (){}'
check_prog_err "fn-no-body"       'fn f()'

# --- closures (step 6b) ---
# The canonical closure: counter keeps state after its maker returned.
check_prog "closure-counter" \
  'fn mk(){let n=0; fn inc(){n=n+1; return n;} return inc;} let c=mk(); print c(); print c(); print c();' \
  "$(printf '1\n2\n3')"
# Independent closures have independent captured state.
check_prog "closure-independent" \
  'fn mk(){let n=0; fn inc(){n=n+1; return n;} return inc;} let a=mk(); let b=mk(); print a(); print a(); print b();' \
  "$(printf '1\n2\n1')"
# Capture a parameter.
check_prog "closure-param" \
  'fn adder(x){fn add(y){return x+y;} return add;} let a=adder(5); print a(3); print a(10);' \
  "$(printf '8\n15')"
# Capture across two levels (chained upvalues).
check_prog "closure-chain" \
  'fn o(){let x=7; fn m(){fn i(){return x;} return i();} return m();} print o();' \
  "7"
# Two closures sharing one variable see each other's writes.
check_prog "closure-shared" \
  'fn mk(){let v=100; fn get(){return v;} fn bump(){v=v+1; return get();} return bump;} let f=mk(); print f(); print f();' \
  "$(printf '101\n102')"
# A closure reading a captured value without mutating it.
check_prog "closure-readonly" \
  'fn mk(msg){fn f(){return msg;} return f;} let g=mk("hi"); print g(); print g();' \
  "$(printf 'hi\nhi')"

# --- static type checker: WELL-TYPED programs run normally (step 7) ---
check_prog "ty-let-int"     'let x: int = 5; print x + 3;'               "8"
check_prog "ty-let-str"     'let s: str = "hi"; print s + "!";'          "hi!"
check_prog "ty-let-bool"    'let b: bool = 1 < 2; print b;'              "true"
check_prog "ty-fn"          'fn add(a: int, b: int): int { return a+b; } print add(3,4);' "7"
check_prog "ty-fn-str"      'fn greet(n: str): str { return "hi " + n; } print greet("x");' "hi x"
check_prog "ty-infer"       'let x: int = 5; let y = x + 1; print y;'    "6"
check_prog "ty-mutual"      'fn ev(n: int): bool { if(n==0) return true; return od(n-1); } fn od(n: int): bool { if(n==0) return false; return ev(n-1); } print ev(6);' "true"
check_prog "ty-return-nil"  'fn f(): nil { return nil; } print f();'     "nil"
# Gradual: `any` (unannotated, or explicit) mixes with typed code freely.
check_prog "ty-gradual-mix" 'let x = 5; let y: int = x; print y + 1;'    "6"
check_prog "ty-gradual-fn"  'fn f(a) { return a + 1; } print f(10);'     "11"
check_prog "ty-any-explicit" 'let a: any = "s"; print a + "x";'          "sx"
# Type names are not reserved words.
check_prog "ty-name-reuse"  'let int = 5; print int + 1;'                "6"

# --- static type checker: ILL-TYPED programs are REJECTED before running ---
check_prog_err "ty-bad-init"      'let x: int = true;'
check_prog_err "ty-int-plus-bool" 'let x: int = 1; let y: bool = false; print x + y;'
check_prog_err "ty-bad-arg"       'fn f(a: int): int { return a; } f(true);'
check_prog_err "ty-bad-arity"     'fn f(a: int): int { return a; } f(1, 2);'
check_prog_err "ty-bad-return"    'fn f(): int { return true; }'
check_prog_err "ty-call-int"      'let x: int = 5; x();'
check_prog_err "ty-neg-bool"      'let b: bool = true; print -b;'
check_prog_err "ty-cmp-str"       'let s: str = "a"; print s < 3;'
check_prog_err "ty-assign-bad"    'let x: int = 1; x = "no";'
check_prog_err "ty-fwd-bad-arg"   'fn a(): int { return b(true); } fn b(x: int): int { return x; }'
check_prog_err "ty-bad-typename"  'let x: integer = 5;'
check_prog_err "ty-concat-int"    'let s: str = "a"; print s + 1;'

# --- optimisation: constant folding must NOT change results (step 8) ---
# These exercise the folder; the answers must equal the unfolded semantics.
check_prog "fold-arith"     'print 2 + 3 * 4;'                "14"
check_prog "fold-nested"    'print (1 + 2) * 3 - 10 / 2;'     "4"
check_prog "fold-unary"     'print --7;'                      "7"
check_prog "fold-not"       'print !!false;'                  "false"
check_prog "fold-not-nil"   'print !nil;'                     "true"
check_prog "fold-not-int"   'print !0;'                       "false"
check_prog "fold-cmp"       'print 10 < 20;'                  "true"
check_prog "fold-eq"        'print 2 == 2;'                   "true"
check_prog "fold-str"       'print "a" + "b" + "c";'          "abc"
check_prog "fold-streq"     'print "ab" == "a" + "b";'        "true"
# Division by zero must NOT be folded away — still a runtime error.
check_prog_err "fold-div0"  'print 1 / 0;'
# Folding must not evaluate the short-circuited side (no side effect).
check_prog "fold-shortcct"  'let x = 0; false and (x = 1 + 1); print x;'  "0"
# Folding inside control flow and functions still produces correct behaviour.
check_prog "fold-in-if"     'if (1 + 1 == 2) print "y"; else print "n";'  "y"
check_prog "fold-in-fn"     'fn f(){ return 6 * 7; } print f();'          "42"
# Constant-condition conditionals/logicals collapse, but to the SAME result and
# with the SAME side effects as the unfolded program (step 46).
check_prog "fold-cond-true"  'print true ? "a" : "b";'                    "a"
check_prog "fold-cond-false" 'print false ? "a" : "b";'                   "b"
check_prog "fold-cond-side"  'let x=0; false ? (x=1) : (x=2); print x;'   "2"
check_prog "fold-and-true"   'print true and 7;'                          "7"
check_prog "fold-and-false"  'print false and 7;'                         "false"
check_prog "fold-or-true"    'print true or 7;'                           "true"
check_prog "fold-or-false"   'print false or 7;'                          "7"
check_prog "fold-or-side"    'let x=0; true or (x=99); print x;'          "0"
check_prog "fold-and-side"   'let x=0; false and (x=99); print x;'        "0"
check_prog "fold-in-lambda"  'let g = fn() => 2 + 3 * 4; print g();'      "14"
check_prog "fold-cond-nested" 'let n=5; print n>0 ? "pos" : "neg";'       "pos"

# --- bytecode peephole optimiser (step 54) ---
# Behaviour must be IDENTICAL with the pass on; the key risk is jump remapping,
# so these place deletable dead pushes around loops, ifs, and recursion.
check_prog "peep-deadpush"  '1; 2; 3; print 9;'                          "9"
check_prog "peep-in-loop"   'let s=0; for (let i in 0..5) { 99; s += i; } print s;' "10"
check_prog "peep-in-if"     'let x=7; if (x>0) { 0; print "y"; } else { 1; print "n"; }' "y"
check_prog "peep-in-fn"     'fn f(n){ 7; if (n<2){ 0; return n; } return f(n-1)+f(n-2); } print f(15);' "610"
check_prog "peep-dbl-not"   'let b = true; print !!b;'                    "true"
check_prog "peep-while"     'let i=0; let n=0; while (i<100) { 0; i+=1; n+=1; } print n;' "100"
check_native "nat-peep"     'fn f(): int { 1; 2; return 42; } print f();' "42"

# --- global inline cache (step 55): must stay correct under every pattern ---
# A mutated global is seen on the next (cached) read.
check_prog "gic-mutate"     'let g=10; fn use(){ return g; } print use(); g=99; print use();' "$(printf '10\n99')"
# A global defined AFTER the function that reads it (resolved at call time).
check_prog "gic-forward"    'fn use(){ return g; } let g=5; print use();' "5"
# Defining MORE globals (which may rebuild the table) must invalidate the cache.
check_prog "gic-after-def"  'let a=1; fn get(){ return a; } print get(); let b=2; let c=3; let d=4; let e=5; let f=6; let h=7; let i=8; let j=9; print get();' "$(printf '1\n1')"
# Redefining a global over an existing name, then reading.
check_prog "gic-redef"      'let x=1; fn r(){ return x; } print r(); let x=2; print r();' "$(printf '1\n2')"
# A hot function called many times reuses its cache (correctness under reuse).
check_prog "gic-hot"        'let base=100; fn add(n){ return base+n; } let s=0; for (let k in 0..5) { s += add(k); } print s;' "510"
# A heavily-reused name is fine (constant dedup keeps it to one slot).
check_prog "dedup-reuse"    'let c = 0; c = c + 1; c = c + 1; c = c + 1; print c;' "3"

# --- caret diagnostics (step 50) ---
# A syntax error renders the offending source line and underlines the token.
check_diag "diag-line"   'let x = 1 +;'        '1 | let x = 1 +;'
check_diag "diag-caret"  'let x = 1 +;'        '^'
check_diag "diag-token"  'let y = 1 + + 2;'    'Error at'
# A lexer error (unterminated string) still shows the line (no caret needed).
check_diag "diag-lexline" 'print "oops;'        'print "oops;'

# --- --stats profiling (step 53) ---
# Running under --stats prints an instruction count and an opcode histogram.
printf 'fn f(n){ if(n<2){return n;} return f(n-1)+f(n-2); } print f(10);' > "$tmp"
stats_out="$("$CNANO" --stats "$tmp" 2>&1)"
case "$stats_out" in
  *"instructions executed"*"opcode histogram"*"CALL"*)
    printf '  ok   %-22s --stats summary ok\n' "stats-summary"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --stats output unexpected:\n%s\n' "stats-summary" "$stats_out"
    fail=$((fail + 1)) ;;
esac

# --- GC instrumentation (step 60) ---
# CNANO_GC_TRACE logs each collection; --stats reports GC cycles + peak heap.
printf 'let n=0; for (let i in 0..30000){ let s=[i]; n+=s.len(); } print n;' > "$tmp"
gc_out="$(CNANO_GC_TRACE=1 "$CNANO" "$tmp" 2>&1)"
case "$gc_out" in
  *"[gc] #"*"reclaimed"*)
    printf '  ok   %-22s GC trace ok\n' "gc-trace"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s GC trace missing:\n%s\n' "gc-trace" "$gc_out"; fail=$((fail + 1)) ;;
esac
gc_stats="$("$CNANO" --stats "$tmp" 2>&1)"
case "$gc_stats" in
  *"GC cycles"*"peak live heap"*)
    printf '  ok   %-22s GC stats ok\n' "gc-stats"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s GC stats missing:\n%s\n' "gc-stats" "$gc_stats"; fail=$((fail + 1)) ;;
esac

# --- control-flow graph (step 61) ---
# --cfg prints basic blocks + successor edges, and flags unreachable blocks.
printf 'fn f(n){ if (n<0) { return 0; } let s=0; while(n>0){ s+=n; n-=1; } return s; } print f(3);' > "$tmp"
cfg_out="$("$CNANO" --cfg "$tmp" 2>&1)"
case "$cfg_out" in
  *"CFG: f"*"B0"*"->"*)
    printf '  ok   %-22s --cfg ok\n' "cfg-blocks"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --cfg output unexpected:\n%s\n' "cfg-blocks" "$cfg_out"; fail=$((fail + 1)) ;;
esac
# --cfg reflects the FINAL (optimised) chunk — dead-block elimination (step 63)
# has already removed unreachable blocks, so none should remain.
case "$cfg_out" in
  *"unreachable"*)
    printf '  FAIL %-22s --cfg still shows an unreachable block (DCE missed it)\n' "dce-clean"
    fail=$((fail + 1)) ;;
  *)
    printf '  ok   %-22s no unreachable blocks after DCE\n' "dce-clean"; pass=$((pass + 1)) ;;
esac
# DCE must be behaviour-preserving: dead-code-heavy programs still compute right.
check_prog "dce-correct" 'fn f(n){ if (n<0) { return 0; } let s=0; while(n>0){ s+=n; n-=1; } return s; } print f(4);' "10"
check_prog "dce-ifret"   'fn g(n){ if (n>0) { return "pos"; } else { return "neg"; } print "dead"; } print g(1); print g(-1);' "$(printf 'pos\nneg')"

# --- unreachable-code analysis (step 62) ---
# A statement after a definite control transfer warns (non-fatal: still runs).
check_diag "unreach-return" 'fn f(): int { return 1; print "x"; } print f();' "unreachable code"
check_diag "unreach-break"  'while (true) { break; print "x"; }' "unreachable code"
check_prog "unreach-runs"   'fn f(){ return 7; let z = 9; } print f();' "7"
# No false positive when both branches of an if exit but code follows neither.
check_prog "unreach-none"   'fn f(n){ if (n>0) { return 1; } return 0; } print f(5);' "1"
# A function ending in an exhaustive match (all arms return) must NOT warn/err.
check_prog "unreach-adt"    'struct A{v:int} struct B{v:int} fn g(x: A|B): int { match (x) { is A => return x.v; is B => return x.v; } } print g(A(5));' "5"
check_diag "sug-field"   'struct P { x: int, y: int } let p = P(1,2); print p.xx;' "did you mean 'x'?"
check_diag "sug-global"  'let count = 5; print conut;' "did you mean 'count'?"
check_diag "sug-type"    'enum Color { Red } let c: Colr = Color.Red; print c;' "did you mean 'Color'?"
check_diag "sug-member"  'enum Dir { North, South } print Dir.Norht;' "did you mean 'North'?"
# A name with no near match gets no (misleading) suggestion.
check_diag "sug-none"    'print zzzzqqq;' "undefined variable 'zzzzqqq'"

# --- three-address IR (step 64a) ---
# Straight-line scalar code lowers to named temporaries: every value is a `t<N>`,
# so `1 + 2 * 3` becomes a const/const/const, a multiply, then an add.
printf 'let a = 2 + 3 * 4; let b = a + a; print b;' > "$tmp"
ir_out="$("$CNANO" --ir "$tmp" 2>&1)"
case "$ir_out" in
  *"IR: <script>"*"t0 = const"*" * "*" + "*"store a = t"*"print t"*)
    printf '  ok   %-22s --ir lowering ok\n' "ir-lower"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --ir output unexpected:\n%s\n' "ir-lower" "$ir_out"; fail=$((fail + 1)) ;;
esac
# Code outside the lowerable subset (collections here) is reported, not lowered.
printf 'let a = [1, 2]; print a[0];' > "$tmp"
ir_out2="$("$CNANO" --ir "$tmp" 2>&1)"
case "$ir_out2" in
  *"not straight-line"*)
    printf '  ok   %-22s --ir rejects non-subset\n' "ir-reject"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --ir should skip non-subset:\n%s\n' "ir-reject" "$ir_out2"; fail=$((fail + 1)) ;;
esac
# --- IR constant propagation + folding (step 64b) ---
# `a` folds to 14 and propagates, so `a + a` folds to a constant 28 after opt.
printf 'let a = 2 + 3 * 4; let b = a + a; print b;' > "$tmp"
ir_opt="$("$CNANO" --ir "$tmp" 2>&1)"
case "$ir_opt" in
  *"optimised"*"const 28"*)
    printf '  ok   %-22s --ir folds + propagates\n' "ir-fold"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --ir did not fold to 28:\n%s\n' "ir-fold" "$ir_opt"; fail=$((fail + 1)) ;;
esac
# Soundness: division by zero must NOT be folded (the VM raises it at runtime),
# so the divide survives into the optimised IR.
printf 'let q = 10 / 0; print q;' > "$tmp"
ir_dz="$("$CNANO" --ir "$tmp" 2>&1)"
opt_section="${ir_dz#*optimised}"
case "$opt_section" in
  *" / "*)
    printf '  ok   %-22s --ir leaves /0 unfolded\n' "ir-nofold-div0"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --ir wrongly folded /0:\n%s\n' "ir-nofold-div0" "$ir_dz"; fail=$((fail + 1)) ;;
esac
# --- IR CSE + dead-temp elimination (step 65) ---
# The repeated `n + n` (n unknown) is computed ONCE and shared; the optimised IR
# must contain exactly one `+`. (grep -c counts matching lines in the opt section.)
printf 'fn f(n){ let a = n + n; let b = n + n; print a; print b; }' > "$tmp"
ir_cse="$("$CNANO" --ir "$tmp" 2>&1)"
plus_count="$(printf '%s\n' "${ir_cse##*f (optimised)}" | grep -c ' + ')"
case "$plus_count" in
  1) printf '  ok   %-22s --ir CSE shares n+n\n' "ir-cse"; pass=$((pass + 1)) ;;
  *) printf '  FAIL %-22s --ir CSE expected 1 add, got %s:\n%s\n' "ir-cse" "$plus_count" "$ir_cse"; fail=$((fail + 1)) ;;
esac
# Dead-temp elimination drops the now-dead constant temporaries left by folding.
# Lowered has three `const` temps (2,3,4); after folding+DCE only the two live
# constants (14, 28) survive, and no `load`/`+` remains in the optimised IR.
printf 'let a = 2 + 3 * 4; let b = a + a; print b;' > "$tmp"
ir_dce="$("$CNANO" --ir "$tmp" 2>&1)"
opt_dce="${ir_dce##*script> (optimised)}"
const_count="$(printf '%s\n' "$opt_dce" | grep -c 'const')"
case "$opt_dce" in
  *load*|*" + "*)
    printf '  FAIL %-22s --ir DCE left load/add behind:\n%s\n' "ir-dce" "$ir_dce"; fail=$((fail + 1)) ;;
  *)
    if [ "$const_count" -eq 2 ]; then
      printf '  ok   %-22s --ir DCE drops dead temps\n' "ir-dce"; pass=$((pass + 1))
    else
      printf '  FAIL %-22s --ir DCE expected 2 consts, got %s:\n%s\n' "ir-dce" "$const_count" "$ir_dce"; fail=$((fail + 1))
    fi ;;
esac
# --- block-local optimisation across control flow (step 73) ---
# The optimiser now runs on loops/functions: a constant computed INSIDE a loop
# body folds (4*5 -> 20) -- and the old "optimiser skipped" note is gone.
printf 'let i = 0; while (i < 3) { let k = 4 * 5; print k; i = i + 1; }' > "$tmp"
ir_loop="$("$CNANO" --ir "$tmp" 2>&1)"
opt_loop="${ir_loop##*script> (optimised)}"
case "$ir_loop" in
  *"optimiser skipped"*)
    printf '  FAIL %-22s --ir still skips control flow:\n%s\n' "ir-blocklocal" "$ir_loop"; fail=$((fail + 1)) ;;
  *)
    case "$opt_loop" in
      *"const 20"*) printf '  ok   %-22s --ir folds inside a loop\n' "ir-blocklocal"; pass=$((pass + 1)) ;;
      *) printf '  FAIL %-22s --ir did not fold 4*5 in loop:\n%s\n' "ir-blocklocal" "$ir_loop"; fail=$((fail + 1)) ;;
    esac ;;
esac
# Soundness: across a CALL (which may reassign globals) a variable's constant must
# NOT propagate. `g` is set to 7, then a call, then read: the read must stay a
# load, not fold to 7.
printf 'fn touch() { return 0; } let g = 7; touch(); print g;' > "$tmp"
ir_call="$("$CNANO" --ir "$tmp" 2>&1)"
opt_call="${ir_call##*script> (optimised)}"
case "$opt_call" in
  *"load g"*) printf '  ok   %-22s --ir keeps load across a call\n' "ir-call-clobber"; pass=$((pass + 1)) ;;
  *) printf '  FAIL %-22s --ir wrongly propagated across a call:\n%s\n' "ir-call-clobber" "$ir_call"; fail=$((fail + 1)) ;;
esac
# --- loop-invariant code motion (step 79) ---
# `a * b` never changes inside the loop, so it must move ABOVE the L0 header
# (computed once), while the loop-carried `load i`/`load s` stay inside.
printf 'fn w(a, b, n) { let s = 0; let i = 0; while (i < n) { s = s + a * b; i = i + 1; } return s; } print w(3, 4, 5);' > "$tmp"
ir_licm="$("$CNANO" --ir "$tmp" 2>&1)"
opt_w="$(printf '%s\n' "$ir_licm" | sed -n '/w (optimised)/,$p')"
pre_loop="$(printf '%s\n' "$opt_w" | sed -n '1,/L0:/p')"
in_loop="$(printf '%s\n' "$opt_w" | sed -n '/L0:/,/goto L0/p')"
case "$pre_loop" in
  *" * "*)
    case "$in_loop" in
      *" * "*) printf '  FAIL %-22s mul still inside the loop too:\n%s\n' "ir-licm" "$ir_licm"; fail=$((fail + 1)) ;;
      *) printf '  ok   %-22s --ir hoists a*b out of the loop\n' "ir-licm"; pass=$((pass + 1)) ;;
    esac ;;
  *) printf '  FAIL %-22s a*b was not hoisted:\n%s\n' "ir-licm" "$ir_licm"; fail=$((fail + 1)) ;;
esac
# Speculation safety: an invariant DIVISION is NOT hoisted (it can fault, and the
# loop may run zero times) — it must remain between L0 and the back-edge.
printf 'fn f(d, n) { let s = 0; let i = 0; while (i < n) { s = s + 100 / d; i = i + 1; } return s; } print f(5, 3);' > "$tmp"
ir_div="$("$CNANO" --ir "$tmp" 2>&1)"
div_pre="$(printf '%s\n' "$ir_div" | sed -n '/f (optimised)/,/L0:/p')"
div_in="$(printf '%s\n' "$ir_div" | sed -n '/f (optimised)/,$p' | sed -n '/L0:/,/goto L0/p')"
case "$div_pre" in
  *" / "*) printf '  FAIL %-22s division was hoisted (unsafe):\n%s\n' "ir-licm-div" "$ir_div"; fail=$((fail + 1)) ;;
  *)
    case "$div_in" in
      *" / "*) printf '  ok   %-22s --ir keeps / inside the loop\n' "ir-licm-div"; pass=$((pass + 1)) ;;
      *) printf '  FAIL %-22s division vanished:\n%s\n' "ir-licm-div" "$ir_div"; fail=$((fail + 1)) ;;
    esac ;;
esac
# Regression (found by LICM's --ir output): CSE must never run a LABEL id in a
# jump through the temp-representative table — the loop's exit branch must still
# target L1, not L0.
case "$ir_licm" in
  *"goto L1"*) printf '  ok   %-22s branch labels survive CSE\n' "ir-label-int"; pass=$((pass + 1)) ;;
  *) printf '  FAIL %-22s exit branch label corrupted:\n%s\n' "ir-label-int" "$ir_licm"; fail=$((fail + 1)) ;;
esac

# --- return-type inference (step 66) ---
# An unannotated function's return type is inferred from its body and ENFORCED at
# call sites: `add` infers int, so assigning its result to a bool is an error
# (under the old gradual rule it was `any` and slipped through).
check_diag "inf-enforced" 'fn add(a: int, b: int) { return a + b; } let bad: bool = add(1,2); print bad;' \
  "is int but variable is declared bool"
# A correct use of the same inferred type still runs.
check_prog "inf-runs"     'fn add(a: int, b: int) { return a + b; } print add(2,3);' "5"
# Soundness: a function that can fall off the end is inferred T|nil, so using its
# result where a non-nil int is required is rejected.
check_diag "inf-nil-union" 'fn maybe(n: int) { if (n > 0) { return n; } } let x: int = maybe(1); print x;' \
  "variable is declared int"
# A function that returns on every path keeps the precise (non-nil) type.
check_prog "inf-allpaths"  'fn sign(n: int) { if (n < 0) { return -1; } else { return 1; } } let x: int = sign(5); print x;' "1"

# --- generics: parametric polymorphism (step 67) ---
# A generic identity function: the result type is the argument type, per call.
check_prog "gen-id-int"   'fn id<T>(x: T): T { return x; } print id(42);' "42"
check_prog "gen-id-str"   'fn id<T>(x: T): T { return x; } print id("hi");' "hi"
# The SOLVED return type is enforced at the call site (int here, not the old any).
check_diag "gen-enforced" 'fn id<T>(x: T): T { return x; } let b: bool = id(42); print b;' \
  "is int but variable is declared bool"
# One type variable shared across parameters; correct uses run.
check_prog "gen-pick"     'fn pick<T>(c: bool, a: T, b: T): T { if (c) { return a; } return b; } print pick(false, 10, 20);' "20"
# A generic body needs no annotation: the return type is inferred as the variable.
check_prog "gen-inferred" 'fn id<T>(x: T) { return x; } print id(99);' "99"
# T can itself be a container type (id over an array), solved end to end.
check_prog "gen-array"    'fn id<T>(x: T): T { return x; } let xs = id([1,2,3]); print xs[1];' "2"
# A type variable NESTED inside a parameter ([T]) is solved from the element type.
check_prog "gen-nested"   'fn head<T>(xs: [T]): T { return xs[0]; } print head([10,20,30]);' "10"
check_diag "gen-nested-enf" 'fn head<T>(xs: [T]): T { return xs[0]; } let b: bool = head([1,2]); print b;' \
  "is int but variable is declared bool"
# Two variables nested in a map parameter, both solved.
check_prog "gen-map"      'fn pairUp<K, V>(k: K, v: V): {K: V} { return {k: v}; } let m = pairUp("age", 42); print m["age"];' "42"
# The native backend has no parametric polymorphism, so it REJECTS generics.
check_native_err "gen-native-reject" 'fn id<T>(x: T): T { return x; } print id(5);'

# --- x86-64 assembly backend (step 69) ---
# Emit real machine code from the IR, assemble it, run it, and check it agrees
# with the VM. Covers instruction selection, the printf calling convention, and
# (in asm-spill) the register allocator's spill path.
check_asm "asm-arith"   'print 2 + 3 * 4;'                              "14"
check_asm "asm-vars"    'let a = 10; let b = a * a; print a; print b;'  "$(printf '10\n100')"
check_asm "asm-ops"     'let x = 100; print x / 7; print x % 7; print x - 1;' "$(printf '14\n2\n99')"
check_asm "asm-bitwise" 'print (12 & 10) | (1 << 4);'                   "$(printf '24')"
check_asm "asm-neg"     'let n = 5; print -n; print ~n;'                "$(printf -- '-5\n-6')"
# A right-nested expression keeps 6 temps live at once (> 5 registers), so the
# allocator must SPILL — and the result must still be correct.
check_asm "asm-spill"   'let r = 1 + (2 + (3 + (4 + (5 + (6 + 7))))); print r; print r * 3;' "$(printf '28\n84')"
# Control flow (step 70): loops and branches compiled to native jumps.
check_asm "asm-while"   'let n=10; let i=1; let s=0; while (i<n) { s=s+i; i=i+1; } print s;' "45"
check_asm "asm-if"      'let n=7; if (n % 2 == 0) { print 0; } else { print 1; } print n;' "$(printf '1\n7')"
check_asm "asm-for"     'let f=1; for (let k=1; k<6; k=k+1) { f=f*k; } print f;' "120"
check_asm "asm-nested"  'let m=0; let j=0; while (j<5) { if (j>m) { m=j; } j=j+1; } print m;' "4"
check_asm "asm-true"    'if (true) { print 42; } else { print 0; }' "42"
# Floats (step 78): SSE arithmetic, and `print` matching the VM's formatFloat
# exactly (".0" appended to whole values; nan/inf spelled by hand).
check_asm "asm-float"      'let pi = 3.14159; let r = 2.0; print pi * r * r;' "12.5664"
check_asm "asm-float-fmt"  'print 3.0; print 10.0 / 4.0; print 0.1 + 0.2;' "$(printf '3.0\n2.5\n0.3')"
check_asm "asm-float-ieee" 'print 7.0 / 0.0; print -7.0 / 0.0; print 0.0 / 0.0;' "$(printf 'inf\n-inf\nnan')"
check_asm "asm-float-mix"  'print 2.5 + 1; print 1 < 1.5; print 1.5 > 2.0;' "$(printf '3.5\ntrue\nfalse')"
check_asm "asm-float-fn"   'fn mean(a, b) { return (a + b) / 2.0; } print mean(2.0, 5.0);' "3.5"
check_asm "asm-float-loop" 'fn growth(p, r, n) { let acc = p; let i = 0; while (i < n) { acc = acc * r; i = i + 1; } return acc; } print growth(100.0, 2.0, 3);' "800.0"
check_asm "asm-float-neg"  'fn absF(x) { if (x < 0.0) { return -x; } return x; } print absF(-2.5);' "2.5"
# nan comparisons are false, like the VM (ucomisd's unordered flags, screened).
check_asm "asm-float-nan"  'let n = 0.0 / 0.0; print n == n; print n < 1.0; print n > 1.0;' "$(printf 'false\nfalse\nfalse')"
# Booleans print as words, exactly like the VM (never the 0/1 underneath).
check_asm "asm-print-bool" 'print 1 < 2; let b = 3 == 4; print b;' "$(printf 'true\nfalse')"
# Logical ! flips the BOOLEAN, not the bits (a notq here once returned -2, truthy).
check_asm "asm-not-bool"   'fn f(n) { if (!(n > 0)) { return 1; } return 2; } print f(5); print f(-5);' "$(printf '2\n1')"
# Still out of subset, still rejected: a variable that is an int on one path and
# a float on another (needs a runtime tag), ! on an int (0 is truthy), and ==
# across classes (the VM says false for int==float; we don't fake it).
check_asm_err "asm-rej-mixedvar" 'let x = 1; x = 2.5; print x;'
check_asm_err "asm-rej-notint"   'let n = 0; if (!n) { print 1; }'
check_asm_err "asm-rej-mixedeq"  'print 1 == 1.0;'

# --- direct ELF emission (step 80): cnano as its own assembler + linker ---
# These binaries are built with NO cc, NO as, NO ld, NO libc — cnano encodes the
# machine bytes and writes the ELF headers itself; print is a raw write syscall.
check_elf "elf-arith"   'print 2 + 3 * 4; print -7; print 100 / 7; print 100 % 7;' "$(printf '14\n-7\n14\n2')"
check_elf "elf-loop"    'let s = 0; let i = 1; while (i < 11) { s = s + i; i = i + 1; } print s;' "55"
check_elf "elf-if"      'let n = 7; if (n % 2 == 0) { print 0; } else { print 1; }' "1"
check_elf "elf-fn"      'fn add(a, b) { return a + b; } print add(40, 2);' "42"
check_elf "elf-fib"     'fn fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print fib(20);' "6765"
check_elf "elf-bool"    'fn isEven(n) { return n % 2 == 0; } print isEven(10); print isEven(7); print !(3 < 1);' "$(printf 'true\nfalse\ntrue')"
check_elf "elf-bits"    'print (1 << 40) + 7; print 12 & 10; print ~5;' "$(printf '1099511627783\n8\n-6')"
# The INT64_MIN edge: negation can't represent it, but the unsigned-magnitude
# decimal conversion still prints it exactly.
check_elf "elf-i64min"  'print 0 - 9223372036854775807 - 1;' "-9223372036854775808"
check_elf_err "elf-rej-float" 'print 1.5;'
check_elf_err "elf-rej-coll"  'let a = [1]; print a[0];'
# cnano's truthiness makes 0 truthy, so branching on a bare int is rejected
# (a zero-test would disagree with the VM).
check_asm_err "asm-rej-intcond"   'let n = 0; while (n) { print 1; }'
# Closures (a nested function capturing a variable) need upvalues -> rejected.
check_asm_err "asm-rej-closure" 'fn mk() { let c = 0; fn inc() { return c; } return inc(); } print mk();'

# Functions, parameters, and calls (step 72): compiled to native with the System
# V ABI and a real call stack -- including recursion. Output must match the VM.
check_asm "asm-call"      'fn add(a, b) { return a + b; } print add(3, 4);' "7"
check_asm "asm-recursion" 'fn fib(n) { if (n < 2) { return n; } return fib(n-1) + fib(n-2); } print fib(10);' "55"
check_asm "asm-fact"      'fn fact(n) { if (n < 2) { return 1; } return n * fact(n-1); } print fact(6);' "720"
check_asm "asm-mutual"    'fn isEven(n){ if(n==0){return 1==1;} return isOdd(n-1); } fn isOdd(n){ if(n==0){return 1==0;} return isEven(n-1); } if (isEven(10)) { print 1; } else { print 0; }' "1"
check_asm "asm-loop-fn"   'fn sumTo(n){ let s=0; let i=1; while(i<n+1){ s=s+i; i=i+1; } return s; } print sumTo(100);' "5050"
check_asm "asm-bool-cond" 'fn pos(n){ return n>0; } fn pick(n){ if(pos(n)){ return n; } return 0-n; } print pick(-8);' "8"

# --- the --types viewer (step 66) ---
# Prints each top-level binding's inferred type, marking inferred returns.
printf 'fn add(a: int, b: int) { return a + b; } fn maybe(n: int) { if (n>0) { return n; } } let s = "hi";' > "$tmp"
ty_out="$("$CNANO" --types "$tmp" 2>&1)"
case "$ty_out" in
  *"fn add(a: int, b: int) : int"*"(inferred)"*"int | nil"*"let s : str"*)
    printf '  ok   %-22s --types ok\n' "types-view"; pass=$((pass + 1)) ;;
  *)
    printf '  FAIL %-22s --types output unexpected:\n%s\n' "types-view" "$ty_out"; fail=$((fail + 1)) ;;
esac

# --- garbage collector (step 10) ---
# Churn: 5000 short-lived closures (+ their upvalues) are allocated and become
# garbage. Correct output here means the GC reclaims them without corrupting the
# loop's live state. Under `make gcstress` (collect on every allocation) this is
# the real torture test for missed roots.
check_prog "gc-closure-churn" \
  'fn mk() { let c = 0; fn inc() { c = c + 1; return c; } return inc; }
   let i = 0; while (i < 5000) { let junk = mk(); i = i + 1; } print i;' "5000"
# Live data must SURVIVE collections: `acc` and its captured upvalue persist while
# thousands of `junk` closures are reclaimed around it. If the GC wrongly freed
# acc, the final count would be wrong (or it would crash under ASan).
check_prog "gc-live-survives" \
  'fn mk() { let c = 0; fn inc() { c = c + 1; return c; } return inc; }
   let acc = mk(); let i = 0; let last = 0;
   while (i < 5000) { let junk = mk(); last = acc(); i = i + 1; } print last;' "5000"

# --- structured type system (step 11) ---
# Collection type annotations parse and are checked STRUCTURALLY: [int] matches
# [int] (and [any]) but not [bool]; {str: int} likewise. The checks recurse into
# element/key/value types and nest arbitrarily.
check_prog "st-array-ok"      'fn id(a: [int]): [int] { return a; } print "ok";' "ok"
check_prog "st-nested-ok"     'fn id(a: [[int]]): [[int]] { return a; } print "ok";' "ok"
check_prog "st-map-ok"        'fn id(m: {str: int}): {str: int} { return m; } print "ok";' "ok"
# `any` still flows freely into a typed collection slot (gradual escape hatch).
check_prog "st-gradual"       'fn need(a: [int]): int { return 0; } fn g(x) { return need(x); } print "ok";' "ok"
# Structural mismatches are rejected (these exit non-zero).
check_prog_err "st-arr-ret-bad"  'fn f(a: [int]): [bool] { return a; } print 1;'
check_prog_err "st-let-arr-int"  'let x: [int] = 5; print x;'
check_prog_err "st-map-val-bad"  'fn f(m: {str: int}): {str: bool} { return m; } print 1;'
check_prog_err "st-map-nest-bad" 'fn f(m: {str: [int]}): {str: int} { return m; } print 1;'
check_prog_err "st-arg-coll-bad" 'fn need(a: [int]): int { return 0; } fn g(b: [bool]): int { return need(b); } print 1;'
check_prog_err "st-bad-syntax"   'let x: [int = 5; print x;'

# --- builtins + method calls (step 12) ---
# Free builtin functions (registered as globals).
check "bi-str-int"      'str(42)'                 "42"
check "bi-str-bool"     'str(true)'               "true"
check "bi-str-nil"      'str(nil)'                "nil"
check_prog "bi-str-concat" 'print "n=" + str(7);' "n=7"
check_prog "bi-clock-int"  'let t = clock(); print t >= 0;' "true"
# Method calls dispatch on the receiver's type (OP_INVOKE).
check "m-str-len"       '"hello".len()'           "5"
check_prog "m-str-len-var" 'let s = "ab" + "cd"; print s.len();' "4"
check_prog "m-str-len-expr" 'print "cnano".len() + 1;' "6"
# Error cases (each exits non-zero).
check_prog_err "m-unknown"   'print "hi".bogus();'
check_prog_err "m-on-int"    'print (5).len();'
check_prog_err "m-bad-arity" 'print "hi".len(1);'
check_prog_err "bi-bad-arity" 'print str();'

# --- parse/char builtins + array reductions (step 45) ---
check "bi-parseint"    'parseInt("42") + 8'      "50"
check "bi-parseint-neg" 'parseInt("  -17 ")'     "-17"
check "bi-parsefloat"  'parseFloat("3.5") * 2.0' "7.0"
check "bi-ord"         'ord("A")'                "65"
check "bi-chr"         'chr(97)'                 "a"
check_prog "bi-ord-chr-roundtrip" 'print chr(ord("z"));' "z"
check "arr-sum"        '[3,1,4,1,5].sum()'       "14"
check "arr-sum-empty"  '[].sum()'                "0"
check "arr-sum-float"  '[1.5, 2, 3].sum()'       "6.5"
check "arr-min"        '[3,1,4,1,5].min()'       "1"
check "arr-max"        '[3,1,4,1,5].max()'       "5"
check_prog "bi-pipeline" 'print "10,20,30".split(",").map(fn(s)=>parseInt(s)).sum();' "60"
check_prog_err "bi-parseint-junk" 'print parseInt("12x");'
check_prog_err "bi-parsefloat-empty" 'print parseFloat("");'
check_prog_err "bi-ord-multi"     'print ord("ab");'
check_prog_err "bi-chr-oob"       'print chr(300);'
check_prog_err "arr-min-empty"    'print [].min();'
check_prog_err "arr-sum-nonnum"   'print [1, "x"].sum();'

# --- for-in iteration (step 17) ---
check_prog "forin-array"  'let s = 0; for (let x in [10,20,30]) { s += x; } print s;' "60"
check_prog "forin-print"  'for (let x in [1,2,3]) print x*x;' "$(printf '1\n4\n9')"
check_prog "forin-map"    'let m = {"a":1,"b":2,"c":3}; let t = 0; for (let k in m) { t += m[k]; } print t;' "6"
check_prog "forin-nested" 'let g = [[1,2],[3,4]]; let s = 0; for (let r in g) { for (let v in r) { s += v; } } print s;' "10"
check_prog "forin-empty"  'let n = 0; for (let x in []) { n += 1; } print n;' "0"
check_prog "forin-string-arr" 'let out = ""; for (let w in ["a","b","c"]) { out += w; } print out;' "abc"
check_prog_err "forin-noncoll" 'for (let x in 5) print x;'
# C-style for must still work after the for-in fork (regression guard).
check_prog "for-cstyle-still" 'let s = 0; for (let i = 0; i < 5; i = i + 1) { s += i; } print s;' "10"
check_prog "for-cstyle-typed" 'let s = 0; for (let i: int = 0; i < 4; i += 1) { s += i; } print s;' "6"

# --- standard library: builtins + string/array/map methods (step 18) ---
check "bi-len-str"   'len("hello")'            "5"
check "bi-len-arr"   'len([1,2,3,4])'          "4"
check "bi-type-int"  'type(5)'                 "int"
check "bi-type-arr"  'type([1])'               "array"
check "bi-type-map"  'type({"a":1})'           "map"
check_prog "bi-type-instance" 'struct Point { x: int } print type(Point(1));' "Point"
check "bi-abs"       'abs(-7)'                 "7"
check "bi-abs-float" 'abs(-2.5)'               "2.5"
check "bi-sqrt"      'sqrt(16.0)'              "4.0"
check "bi-sqrt-int"  'sqrt(9)'                 "3.0"
check "bi-floor"     'floor(3.7)'              "3.0"
check "bi-ceil"      'ceil(3.2)'               "4.0"
check "bi-round"     'round(3.5)'              "4.0"
check "bi-pow"       'pow(2.0, 10.0)'          "1024.0"
check "bi-min-mixed" 'min(3, 2.0)'             "2.0"
check "bi-max-float" 'max(2.5, 1.0)'           "2.5"
check "bi-min"       'min(3, 8)'               "3"
check "bi-max"       'max(3, 8)'               "8"
check_prog "bi-assert-ok"  'assert(1 < 2); print "ok";' "ok"
check_prog_err "bi-assert-fail" 'assert(2 < 1); print "x";'
check "m-str-upper"  '"Hello".upper()'         "HELLO"
check "m-str-lower"  '"Hello".lower()'         "hello"
check "m-str-contains" '"hello".contains("ell")' "true"
check "m-str-indexof"  '"hello".indexOf("l")'  "2"
check "m-str-substr"   '"hello world".substring(6, 11)' "world"
check_prog_err "m-substr-oob" 'print "hi".substring(0, 9);'
check_prog "m-arr-sort" 'let a=[3,1,2]; a.sort(); print a;' "[1, 2, 3]"
check_prog "m-arr-sort-str" 'let w=["c","a","b"]; w.sort(); print w.join("-");' "a-b-c"
check "m-arr-contains" '[1,2,3].contains(2)'   "true"
check "m-arr-indexof"  '[10,20,30].indexOf(30)' "2"
check "m-arr-join"     '[1,2,3].join(", ")'    "1, 2, 3"
check_prog "m-map-values" 'let m={"a":10,"b":20}; let v=m.values(); v.sort(); print v;' "[10, 20]"
check_prog_err "m-sort-mixed" 'let a=[1,"two"]; a.sort(); print a;'

# --- more string/array library methods (step 41) ---
check "m-str-trim"      '"  hi  ".trim()'        "hi"
check "m-str-trim-empty" '"   ".trim().len()'    "0"
check "m-str-starts"    '"hello".startsWith("he")' "true"
check "m-str-starts-no" '"hello".startsWith("lo")' "false"
check "m-str-ends"      '"hello".endsWith("lo")' "true"
check "m-str-ends-long" '"hi".endsWith("ahi")'   "false"
check "m-str-replace"   '"a.b.c".replace(".", "/")' "a/b/c"
check "m-str-replace-grow" '"xx".replace("x", "yy")' "yyyy"
check "m-str-replace-empty" '"abc".replace("", "-")' "abc"
check "m-str-repeat"    '"ab".repeat(3)'         "ababab"
check "m-str-repeat-zero" '"ab".repeat(0).len()' "0"
check_prog "m-str-split"  'print "a,b,c".split(",").join("|");' "a|b|c"
check_prog "m-str-split-chars" 'print "abc".split("").len();' "3"
check_prog "m-str-split-trail" 'print "a,b,".split(",").len();' "3"
check_prog "m-str-split-none" 'print "abc".split(",").join("|");' "abc"
check_prog "m-str-split-roundtrip" 'let p = "x-y-z".split("-"); print p.join("-");' "x-y-z"
check "m-arr-reverse"   '[1,2,3].reverse()'      "[3, 2, 1]"
check_prog "m-arr-reverse-pure" 'let a=[1,2,3]; a.reverse(); print a;' "[1, 2, 3]"
check "m-arr-slice"     '[1,2,3,4,5].slice(1, 4)' "[2, 3, 4]"
check "m-arr-slice-empty" '[1,2,3].slice(1, 1)'  "[]"
check_prog_err "m-arr-slice-oob" 'print [1,2,3].slice(0, 9);'

# --- higher-order collection methods (step 19) ---
# These call a cnano function back from C (re-entrant VM). Under `make gcstress`
# they double as the torture test: the result/accumulator must survive a GC
# triggered inside a callback.
check_prog "ho-map"     'fn d(x){return x*2;} print [1,2,3,4].map(d);' "[2, 4, 6, 8]"
check_prog "ho-map-closure" 'fn mk(){let n=10; fn a(x){return x+n;} return a;} print [1,2,3].map(mk());' "[11, 12, 13]"
check_prog "ho-filter"  'fn ev(x){return x%2==0;} print [1,2,3,4,5,6].filter(ev);' "[2, 4, 6]"
check_prog "ho-reduce"  'fn s(a,b){return a+b;} print [1,2,3,4,5].reduce(s, 0);' "15"
check_prog "ho-chain"   'fn sq(x){return x*x;} fn big(x){return x>5;} fn s(a,b){return a+b;} print [1,2,3,4].map(sq).filter(big).reduce(s,0);' "25"
check_prog "ho-reduce-str" 'fn cat(a,b){return a+str(b);} print [1,2,3].reduce(cat, "n:");' "n:123"
check_prog_err "ho-callback-err" 'fn bad(x){return x/0;} print [1,2].map(bad);'

# --- compound assignment (step 16) ---
check_prog "cmpd-var"    'let x = 10; x += 5; x -= 3; x *= 2; print x;' "24"
check_prog "cmpd-mod"    'let x = 17; x %= 5; print x;' "2"
check_prog "cmpd-str"    'let s = "a"; s += "b"; s += "c"; print s;' "abc"
check_prog "cmpd-array"  'let a = [10, 20, 30]; a[1] += 5; print a[1];' "25"
check_prog "cmpd-map"    'let m = {"n": 1}; m["n"] += 41; print m["n"];' "42"
check_prog "cmpd-nested" 'let m = {"a": [1,2,3]}; m["a"][1] += 100; print m["a"][1];' "102"
check_prog "cmpd-loop"   'let s = 0; let i = 1; while (i <= 5) { s += i; i += 1; } print s;' "15"
check_prog_err "cmpd-type-err" 'let x: int = 1; x += "s"; print x;'
check_native "nat-cmpd"  'fn f(): int { let x: int = 5; x += 3; x *= 2; return x; } print f();' "16"
# Bitwise compound assignment (step 49): &= |= ^= <<= >>=
check_prog "cmpd-and"    'let x = 0b1100; x &= 0b1010; print x;' "8"
check_prog "cmpd-or"     'let x = 8; x |= 1; print x;'           "9"
check_prog "cmpd-xor"    'let x = 9; x ^= 0b1111; print x;'      "6"
check_prog "cmpd-shl"    'let y = 1; y <<= 4; print y;'          "16"
check_prog "cmpd-shr"    'let y = 64; y >>= 2; print y;'         "16"
check_prog "cmpd-bit-idx" 'let a = [12]; a[0] &= 10; print a[0];' "8"
check_prog "cmpd-shl-chain" 'let v = 1; v <<= 2; v <<= 3; print v;' "32"
check_native "nat-cmpd-bit" 'fn f(): int { let x: int = 0xF0; x |= 0x0F; x >>= 4; return x; } print f();' "15"
# Regression: shifts and comparisons must still lex correctly next to '='.
check "cmpd-not-shift"   '1 << 4'                                "16"
check "cmpd-not-le"      '3 <= 3'                                "true"
check_prog_err "cmpd-const-bit" 'const c = 1; c <<= 2; print c;'

# --- arrays (step 13) ---
check "arr-index"        '[10, 20, 30][1]'        "20"
check "arr-len-literal"  '[1, 2, 3].len()'        "3"
check "arr-empty-len"    '[].len()'               "0"
check_prog "arr-print"   'print [1, 2, 3];'       "[1, 2, 3]"
check_prog "arr-set"     'let a = [1,2,3]; a[1] = 99; print a;' "[1, 99, 3]"
check_prog "arr-push-pop" 'let a = [1]; a.push(2); a.push(3); print a.pop(); print a;' "$(printf '3\n[1, 2]')"
check_prog "arr-nested"  'let m = [[1,2],[3,4]]; print m[1][0];' "3"
check_prog "arr-computed" 'let a = [5,6,7]; let i = 1+1; print a[i];' "7"
check_prog "arr-build-loop" 'let a = []; let i = 0; while (i<3) { a.push(i*i); i=i+1; } print a;' "[0, 1, 4]"
check_prog "arr-heterogeneous" 'let a: [any] = [1, "two", true]; print a[1];' "two"
# Typed arrays: element types are checked structurally.
check_prog "arr-typed-ok" 'let a: [int] = [1,2,3]; print a.len();' "3"
check_prog "arr-fn-arg"  'fn n(a: [int]): int { return a.len(); } print n([4,5]);' "2"
check_prog_err "arr-elem-bad"   'let a: [int] = ["x"]; print 1;'
check_prog_err "arr-store-bad"  'let a: [int] = [1]; a[0] = "x"; print 1;'
check_prog_err "arr-idx-type"   'let a: [int] = [1]; print a[true];'
check_prog_err "arr-idx-nonarr" 'let x = 5; print x[0];'
check_prog_err "arr-elem-out"   'let b: bool = [1,2][0]; print b;'
# Runtime errors (non-zero exit).
check_prog_err "arr-oob"        'let a = [1,2]; print a[5];'
check_prog_err "arr-pop-empty"  'let a = []; print a.pop();'
# GC: churn 2000 short-lived arrays while one live array grows to 2000 elements.
# The collector must reclaim the junk yet keep `live` and all its elements — the
# real test that arrays are traced correctly (run under `make gcstress` too).
check_prog "gc-array-live" \
  'let live = []; let i = 0;
   while (i < 2000) { let junk = [i, i+1, i+2]; live.push(i); i = i + 1; }
   print live.len(); print live[1999];' "$(printf '2000\n1999')"

# --- maps (step 14) ---
# General value keys: strings, ints, bools, nil. (Multi-entry print order is
# bucket order, so we assert via indexing/len/has, not by printing the whole map.)
check "map-str-key"      '{"a": 1, "b": 2}["b"]'  "2"
check "map-int-key"      '{1: "one", 2: "two"}[2]' "two"
check "map-bool-key"     '{true: "yes", false: "no"}[false]' "no"
check "map-nil-key"      '{nil: 42}[nil]'         "42"
check "map-len"          '{"x": 1, "y": 2}.len()' "2"
check_prog "map-empty-print" 'print {};'          "{}"
check_prog "map-set-get" 'let m = {"a": 1}; m["b"] = 2; print m["b"]; print m.len();' "$(printf '2\n2')"
check_prog "map-overwrite" 'let m = {"k": 1}; m["k"] = 9; print m["k"]; print m.len();' "$(printf '9\n1')"
check_prog "map-has"     'let m = {"x": 1}; print m.has("x"); print m.has("z");' "$(printf 'true\nfalse')"
check_prog "map-keys"    'let m = {"a": 1, "b": 2, "c": 3}; print m.keys().len();' "3"
check_prog "map-int-loop" 'let m = {}; let i = 0; while (i<5) { m[i] = i*i; i=i+1; } print m[4]; print m.len();' "$(printf '16\n5')"
check_prog "map-nested"  'let m = {"evens": [2,4,6]}; print m["evens"][2];' "6"
# Typed maps: key and value types are checked structurally.
check_prog "map-typed-ok" 'let m: {str: int} = {"a": 1, "b": 2}; print m["a"];' "1"
check_prog "map-typed-nested" 'let m: {str: [int]} = {"a": [1,2,3]}; print m["a"][1];' "2"
check_prog_err "map-key-bad"  'let m: {str: int} = {"a": 1}; print m[1];'
check_prog_err "map-val-bad"  'let m: {str: int} = {"a": 1}; m["a"] = true; print 1;'
check_prog_err "map-lit-bad"  'let m: {str: int} = {"a": "x"}; print 1;'
check_prog_err "map-elem-out" 'let m: {str: int} = {"a": 1}; let b: bool = m["a"]; print b;'
# Runtime errors.
check_prog_err "map-missing"  'let m = {"a": 1}; print m["b"];'
check_prog_err "map-nonhash"  'let m = {}; m[[1,2]] = 3; print 1;'
# GC: churn 2000 short-lived maps while one live map grows to 2000 entries. The
# collector must trace map keys AND values (run under `make gcstress`).
check_prog "gc-map-live" \
  'let live = {}; let i = 0;
   while (i < 2000) { let junk = {i: i * 2}; live[i] = i; i = i + 1; }
   print live.len(); print live[1999];' "$(printf '2000\n1999')"

# --- structs / records (step 20) ---
check_prog "struct-basic"   'struct Point { x: int, y: int } let p = Point(3, 4); print p.x + p.y;' "7"
check_prog "struct-print"   'struct P { x: int, y: int } print P(1, 2);' "P{x: 1, y: 2}"
check_prog "struct-set"     'struct C { n: int } let c = C(0); c.n = 10; c.n += 5; print c.n;' "15"
check_prog "struct-nested"  'struct I { v: int } struct O { inner } let o = O(I(42)); print o.inner.v;' "42"
check_prog "struct-array"   'struct P { x: int } let a = [P(1), P(2)]; a[0].x = 99; print a[0].x; print a[1].x;' "$(printf '99\n2')"
check_prog "struct-fn"      'struct P { x: int, y: int } fn sx(p: P): int { return p.x; } print sx(P(10, 20));' "10"
check_prog "struct-typed"   'struct Pt { x: int, y: int } let p: Pt = Pt(3, 4); print p.x + p.y;' "7"
# Runtime errors.
check_prog_err "struct-no-field"   'struct P { x: int } let p = P(1); print p.z;'
check_prog_err "struct-arity-rt"   'fn f(p) { return p; } struct P { x: int, y: int } let g = f(P); g(1);'
check_prog_err "struct-field-nonobj" 'let p = 5; print p.x;'
# Static type errors.
check_prog_err "struct-arg-type"   'struct P { x: int, y: int } let p = P(3, "no"); print p;'
check_prog_err "struct-arity-st"   'struct P { x: int, y: int } let p = P(3); print p;'
check_prog_err "struct-field-flow" 'struct P { x: int } let p = P(1); let b: bool = p.x; print b;'
check_prog_err "struct-store-bad"  'struct P { x: int } let p = P(1); p.x = "no"; print p;'
check_prog_err "struct-unknown-ty" 'let p: Nope = 5; print p;'
check_prog_err "struct-nominal"    'struct A { v: int } struct B { v: int } fn f(a: A): int { return a.v; } print f(B(1));'
check_native_err "nat-rej-struct"  'struct P { x: int } let p = P(1); print p.x;'

# --- enums (step 44) ---
check_prog "enum-access"    'enum Color { Red, Green, Blue } print Color.Green;' "Color.Green"
check_prog "enum-eq"        'enum C { A, B } print C.A == C.A; print C.A == C.B;' "$(printf 'true\nfalse')"
check_prog "enum-distinct"  'enum X { A } enum Y { A } print X.A == Y.A;' "false"
check_prog "enum-type"      'enum Color { Red } print type(Color.Red);' "Color"
check_prog "enum-match"     'enum C { Red, Green, Blue } fn n(c){ match (c) { C.Red => return "r"; C.Green => return "g"; _ => return "?"; } } print n(C.Red); print n(C.Blue);' "$(printf 'r\n?')"
check_prog "enum-typed-fn"  'enum C { Red, Blue } fn warm(c: C): bool { return c == C.Red; } print warm(C.Red); print warm(C.Blue);' "$(printf 'true\nfalse')"
check_prog "enum-typed-let" 'enum C { Red, Blue } let c: C = C.Blue; print c;' "C.Blue"
check_prog "enum-in-array"  'enum C { Red, Blue } let p = [C.Red, C.Blue]; print p.contains(C.Blue); print p[0];' "$(printf 'true\nC.Red')"
check_prog "enum-trailing-comma" 'enum C { A, B, } print C.B;' "C.B"
# Errors: unknown member (compile time), nominal mismatch, runtime unknown member.
check_prog_err "enum-bad-member"  'enum C { A, B } print C.Z;'
check_prog_err "enum-type-mismatch" 'enum C { A } enum D { X } let c: C = D.X; print c;'
check_prog_err "enum-empty"       'enum C { } print 1;'
check_prog_err "enum-as-int"      'enum C { A } let n: int = C.A; print n;'
check_native_err "nat-rej-enum"   'enum C { A, B } fn f(): int { return 1; } print f();'

# --- match exhaustiveness (step 56) ---
# A match over an enum/bool variable must cover every case (or have `_`).
check_prog "exh-enum-full"  'enum C { Red, Green, Blue } let c = C.Green; match (c) { C.Red => print 1; C.Green => print 2; C.Blue => print 3; }' "2"
check_prog_err "exh-enum-missing" 'enum C { Red, Green, Blue } let c = C.Red; match (c) { C.Red => print 1; C.Green => print 2; }'
check_prog "exh-enum-default" 'enum C { Red, Green, Blue } let c = C.Blue; match (c) { C.Red => print 1; _ => print 0; }' "0"
check_prog "exh-bool-full"  'let b = false; match (b) { true => print 1; false => print 0; }' "0"
check_prog_err "exh-bool-missing" 'let b = true; match (b) { true => print 1; }'
# An open type (int) is not closed, so no `_` is required.
check_prog "exh-int-open"   'let n = 2; match (n) { 1 => print 1; 2 => print 2; }' "2"
# A non-variable subject is not analysed (we can't pin its type).
check_prog "exh-nonvar"     'enum C { A, B } fn pick(): C { return C.A; } match (pick()) { C.A => print 1; }' "1"
# The redundant-'_'-arm warning is non-fatal: the program still runs.
check_prog "exh-redundant-runs" 'enum C { A, B } let c = C.A; match (c) { C.A => print 1; C.B => print 2; _ => print 0; }' "1"
check_diag "exh-redundant-warn" 'enum C { A, B } let c = C.A; match (c) { C.A => print 1; C.B => print 2; _ => print 0; }' "unreachable"
check_diag "exh-missing-names"  'enum C { Red, Green, Blue } let c = C.Red; match (c) { C.Red => print 1; }' "missing C.Green, C.Blue"

# --- tagged-union ADTs: struct variants + union + exhaustive `is`-match (step 57) ---
check_prog "adt-area"       'struct Circle { r: int } struct Rect { w: int, h: int } fn area(s: Circle|Rect): int { match (s) { is Circle => return s.r*s.r; is Rect => return s.w*s.h; } } print area(Circle(3)); print area(Rect(2,4));' "$(printf '9\n8')"
check_prog "adt-three"      'struct A{} struct B{} struct C{} fn tag(x: A|B|C): str { match (x) { is A => return "a"; is B => return "b"; is C => return "c"; } } print tag(B());' "b"
check_prog_err "adt-missing" 'struct Circle { r: int } struct Rect { w: int, h: int } fn area(s: Circle|Rect): int { match (s) { is Circle => return s.r; } } print area(Circle(1));'
check_prog "adt-default"    'struct A{v:int} struct B{v:int} fn f(x: A|B): int { match (x) { is A => return 1; _ => return 0; } } print f(B(9));' "0"
check_diag "adt-missing-name" 'struct Circle { r: int } struct Rect { w: int, h: int } fn a(s: Circle|Rect): int { match (s) { is Circle => return s.r; } } print a(Circle(1));' "missing Rect"
check_diag "adt-redundant"  'struct A{v:int} struct B{v:int} fn f(x: A|B): int { match (x) { is A => return 1; is B => return 2; _ => return 0; } } print f(A(1));' "unreachable"

# --- methods on structs (step 21) ---
check_prog "method-self"    'struct P { x: int, y: int fn sum(): int { return self.x + self.y; } } print P(3,4).sum();' "7"
check_prog "method-mutate"  'struct C { n: int fn inc(by: int) { self.n += by; } } let c = C(0); c.inc(5); c.inc(3); print c.n;' "8"
check_prog "method-calls-method" 'struct R { w: int, h: int fn area(): int { return self.w*self.h; } fn d(): str { return "a=" + str(self.area()); } } print R(3,4).d();' "a=12"
check_prog "method-on-elem" 'struct P { x: int fn dbl(): int { return self.x*2; } } let a = [P(1), P(5)]; print a[1].dbl();' "10"
check_prog_err "method-unknown"   'struct P { x: int } print P(1).nope();'
check_prog_err "method-body-type" 'struct P { x: int fn bad(): str { return self.x; } } print P(1).bad();'
check_prog_err "method-self-field" 'struct P { x: int fn f(): int { return self.z; } } print P(1).f();'

# --- constructors / init (step 22) ---
check_prog "init-basic"     'struct T { c: int fn init(v: int) { self.c = v; } } print T(100).c;' "100"
check_prog "init-derived"   'struct Circle { r: int, area: int fn init(radius: int) { self.r = radius; self.area = 3*radius*radius; } } let c = Circle(10); print c.area;' "300"
check_prog "init-arity"     'struct R { lo: int, hi: int, span: int fn init(a: int, b: int) { self.lo=a; self.hi=b; self.span=b-a; } } print R(3, 10).span;' "7"
check_prog "init-and-method" 'struct Acct { bal: int fn init(b: int) { self.bal = b; } fn deposit(x: int) { self.bal += x; } } let a = Acct(50); a.deposit(25); print a.bal;' "75"
check_prog "no-init-positional" 'struct P { x: int, y: int } let p = P(1, 2); print p.x + p.y;' "3"
check_prog_err "init-return-val"  'struct P { x: int fn init(v: int) { self.x = v; return 5; } } let p = P(1); print p;'
check_prog_err "init-arg-type"    'struct T { c: int fn init(v: int) { self.c = v; } } let t = T("hot"); print t;'
check_prog_err "init-arity-err"   'struct T { c: int fn init(v: int) { self.c = v; } } let t = T(1, 2); print t;'

# --- match / value dispatch (step 33) ---
check_prog "match-int"   'fn n(x){ match (x) { 0 => return "zero"; 1 => return "one"; _ => return "many"; } } print n(0); print n(9);' "$(printf 'zero\nmany')"
check_prog "match-str"   'let c = "stop"; match (c) { "go" => print 1; "stop" => print 0; _ => print -1; }' "0"
check_prog "match-block" 'let x = 2; match (x) { 1 => print "a"; 2 => { print "two"; print "!"; } }' "$(printf 'two\n!')"
check_prog "match-expr-subject" 'match (3 + 4) { 7 => print "seven"; _ => print "no"; }' "seven"
check_prog "match-no-default" 'match (5) { 1 => print "x"; } print "after";' "after"
check_prog "match-in-loop" 'for (let i in 0..5) { match (i) { 3 => break; _ => print i; } }' "$(printf '0\n1\n2')"
# Type-pattern arms (`is TYPE =>`): the matched arm narrows the subject variable.
check_prog "match-type-int" 'fn d(v: int|str): str { match (v) { is int => return "i${v*2}"; is str => return "s${v.upper()}"; _ => return "?"; } } print d(5); print d("hi");' "$(printf 'i10\nsHI')"
check_prog "match-type-mixed" 'fn d(n: int): str { match (n) { 0 => return "zero"; is int => return "n${n+1}"; _ => return "?"; } } print d(0); print d(7);' "$(printf 'zero\nn8')"
check_prog "match-type-nullable" 'fn g(s: str?): str { match (s) { is str => return "got:${s}"; _ => return "none"; } } print g("ok"); print g(nil);' "$(printf 'got:ok\nnone')"
check_prog "match-type-bool" 'fn k(v: int|bool): str { match (v) { is bool => return "b${v}"; _ => return "i${v}"; } } print k(true); print k(3);' "$(printf 'btrue\ni3')"
check_native "nat-match" 'fn c(n: int): int { match (n) { 0 => return 100; 1 => return 200; _ => return 0; } } print c(1);' "200"

# --- modules / imports (step 40) ---
# Basic import: the entry file uses a function defined in another file.
check_module "mod-basic" "main.cn" "25" \
  "main.cn" 'import "math.cn"; print square(5);' \
  "math.cn" 'fn square(n: int): int { return n * n; }'
# Relative paths: an import resolves relative to the importing file's directory,
# and a nested import (lib/dep.cn) resolves relative to lib/.
check_module "mod-relative" "main.cn" "$(printf 'Hi Ada\n9')" \
  "main.cn" 'import "lib/api.cn"; print greet("Ada"); print sq(3);' \
  "lib/api.cn" 'import "util.cn"; fn greet(n: str): str { return "Hi " + n; }' \
  "lib/util.cn" 'fn sq(n: int): int { return n * n; }'
# Diamond: math.cn is imported via two paths but its definitions appear once
# (no "already defined" error), and a top-level const carries across.
check_module "mod-diamond" "main.cn" "$(printf '49\n3')" \
  "main.cn" 'import "a.cn"; import "math.cn"; print square(7); print PI;' \
  "a.cn" 'import "math.cn"; fn unused(): int { return PI; }' \
  "math.cn" 'fn square(n: int): int { return n * n; } const PI = 3;'
# Cycle: a imports b, b imports a — the once-only guard makes this terminate.
check_module "mod-cycle" "c.cn" "3" \
  "c.cn" 'import "a.cn"; print fa() + fb();' \
  "a.cn" 'import "b.cn"; fn fa(): int { return 1; }' \
  "b.cn" 'import "a.cn"; fn fb(): int { return 2; }'
# Imported types are checked across the boundary (struct from another file).
check_module "mod-struct" "main.cn" "12" \
  "main.cn" 'import "geo.cn"; let r = Rect(3, 4); print r.area();' \
  "geo.cn" 'struct Rect { w: int, h: int  fn area(): int { return self.w * self.h; } }'
# Errors: a missing import, and a misplaced (nested) import.
check_module_err "mod-missing" "main.cn" \
  "main.cn" 'import "nope.cn"; print 1;'
check_module_err "mod-nested" "main.cn" \
  "main.cn" 'fn f(): int { import "x.cn"; return 1; } print f();' \
  "x.cn" 'fn g(): int { return 0; }'
# A type error in an imported file is reported (the whole program is checked).
check_module_err "mod-typeerr" "main.cn" \
  "main.cn" 'import "bad.cn"; print 1;' \
  "bad.cn" 'fn f(): int { return "not an int"; }'
# Per-file positions (step 52): an error names which imported file it came from.
check_module_diag "mod-pos-type" "main.cn" "bad.cn:1" \
  "main.cn" 'import "bad.cn"; print 1;' \
  "bad.cn" 'fn f(): int { return "no"; }'
check_module_diag "mod-pos-runtime" "main.cn" "boom.cn:1" \
  "main.cn" 'import "boom.cn"; print boom();' \
  "boom.cn" 'fn boom(): int { return 1 / 0; }'
check_module_diag "mod-pos-root" "main.cn" "main.cn:1" \
  "main.cn" 'import "x.cn"; let z: int = "no";' \
  "x.cn" 'fn unused(): int { return 0; }'

# --- the standard prelude, written in cnano itself (step 75) ---
# These import the REAL std/prelude.cn (by absolute path), so they test the
# shipped file, not a copy — the self-hosting milestone exercised end to end.
PRELUDE="$(cd "$(dirname "$0")/../std" && pwd)/prelude.cn"
check_prog "pre-gcd"     "import \"$PRELUDE\"; print gcd(54, 24);" "6"
check_prog "pre-lcm"     "import \"$PRELUDE\"; print lcm(4, 6);" "12"
check_prog "pre-ipow"    "import \"$PRELUDE\"; print ipow(3, 13);" "1594323"
check_prog "pre-clamp"   "import \"$PRELUDE\"; print clamp(99, 0, 10); print clamp(-1, 0, 10);" "$(printf '10\n0')"
check_prog "pre-range"   "import \"$PRELUDE\"; print range(2, 7).join(\",\"); print rangeBy(10, 0, -3).join(\",\");" "$(printf '2,3,4,5,6\n10,7,4,1')"
# sortBy: a DESCENDING comparator — the builtin .sort() can't express this.
check_prog "pre-sortby"  "import \"$PRELUDE\"; print sortBy([3,1,4,1,5,9], fn(a, b) => a > b).join(\",\");" "9,5,4,3,1,1"
# A named function (isEven) used as a first-class predicate.
check_prog "pre-countif" "import \"$PRELUDE\"; print countIf(range(1, 101), isEven);" "50"
check_prog "pre-anyall"  "import \"$PRELUDE\"; print any([1,3,5], isEven); print all([2,4,6], isEven);" "$(printf 'false\ntrue')"
check_prog "pre-unique"  "import \"$PRELUDE\"; print unique([1,2,1,3,2]).join(\",\");" "1,2,3"
# The generic first<T>/last<T> keep their element type (a [str] yields a str).
check_prog "pre-generic" "import \"$PRELUDE\"; print first([10,20]) + 1; print last([\"a\",\"b\"]).upper();" "$(printf '11\nB')"
check_prog "pre-strings" "import \"$PRELUDE\"; print padLeft(\"7\", 4, \"0\"); print capitalize(\"hello\"); print reverseStr(\"cnano\");" "$(printf '0007\nHello\nonanc')"

# --- string escape sequences (step 34) ---
check_prog "esc-newline"   'print "a\nb";' "$(printf 'a\nb')"
check_prog "esc-tab-len"   'print "x\ty".len();' "3"
check_prog "esc-quote"     'print "say \"hi\"";' 'say "hi"'
check_prog "esc-backslash" 'print "a\\b".len();' "3"
check_prog "esc-dollar"    'let x=5; print "\${x}=${x}";' '${x}=5'
check_native "nat-esc"     'fn g(): str { return "a\nb\"c"; } print g();' "$(printf 'a\nb"c')"

# --- string interpolation (step 32) ---
check_prog "interp-basic"  'let name = "world"; print "Hello, ${name}!";' "Hello, world!"
check_prog "interp-expr"   'let a=3; let b=4; print "${a} + ${b} = ${a+b}";' "3 + 4 = 7"
check_prog "interp-convert" 'print "n=${42}, b=${true}, x=${nil}";' "n=42, b=true, x=nil"
check_prog "interp-field"  'struct P { x: int, y: int } let p=P(3,4); print "(${p.x}, ${p.y})";' "(3, 4)"
check_prog "interp-method" 'print "len ${[1,2,3].len()}";' "len 3"
check_prog "interp-adjacent" 'let x=7; print "${x}${x}";' "77"
check_prog "interp-index" 'let m={1:99}; print "v=${m[1]}";' "v=99"
check_prog "interp-plain-dollar" 'print "costs $5 (not interpolated)";' 'costs $5 (not interpolated)'

# --- union types + `is` narrowing (step 31) ---
check "is-int"          '5 is int'                 "true"
check "is-str-false"    '5 is str'                 "false"
check "is-array"        '[1,2] is [any]'           "true"
check "is-map"          '{"a":1} is {str:int}'     "true"
check_prog "is-struct"  'struct P { x: int } struct Q { y: int } let p = P(1); print p is P; print p is Q;' "$(printf 'true\nfalse')"
check_prog "union-assign" 'let x: int | str = 5; print x; let y: int | str = "hi"; print y;' "$(printf '5\nhi')"
check_prog "union-narrow" 'fn d(v: int | str): str { if (v is int) { return "i:" + str(v*2); } return "s:" + v; } print d(21); print d("hi");' "$(printf 'i:42\ns:hi')"
check_prog "union-nil-narrow" 'fn g(v: int | nil): int { if (v is int) { return v; } return -1; } print g(7); print g(nil);' "$(printf '7\n-1')"
check_prog_err "union-bad-assign" 'let x: int | str = true; print x;'
check_prog_err "union-no-narrow"  'fn f(v: int | str): int { return v * 2; } print f(5);'
check_native_err "nat-rej-union"  'fn f(v: int | str): int { return 0; } print f(5);'

# --- nullable / optional types (step 24) ---
check_prog "null-assign"   'let a: int? = nil; let b: int? = 5; print b;' "5"
check_prog "null-narrow-then" 'fn f(x: int?): int { if (x != nil) { return x; } return -1; } print f(7); print f(nil);' "$(printf '7\n-1')"
check_prog "null-narrow-else" 'fn g(x: int?): int { if (x == nil) { return 0; } else { return x; } } print g(42);' "42"
check_prog "null-struct"   'struct P { x: int } fn f(p: P?): int { if (p != nil) { return p.x; } return -1; } print f(P(9)); print f(nil);' "$(printf '9\n-1')"
check_prog_err "null-use-as-t"  'let a: int? = nil; let b: int = a; print b;'
check_prog_err "null-wrong-inner" 'let a: int? = "no"; print a;'
check_prog_err "null-no-narrow"  'fn f(x: int?): int { return x; } print f(1);'
check_native_err "nat-rej-nullable" 'fn f(x: int?): int { return 0; } print f(nil);'

# --- const / immutable bindings (step 30) ---
check_prog "const-value"   'const PI = 3; print PI * 2;' "6"
check_prog "const-typed"   'const MAX: int = 100; print MAX;' "100"
check_prog "const-contents-mutable" 'const a = [1,2,3]; a[0] = 99; print a;' "[99, 2, 3]"
check_prog_err "const-reassign"  'const x = 5; x = 6; print x;'
check_prog_err "const-compound"  'const x = 5; x += 1; print x;'
check_prog_err "const-local"     'fn f(): int { const n = 10; n = 20; return n; } print f();'
check_native "nat-const" 'fn f(): int { const base: int = 100; return base + 1; } print f();' "101"

# --- collection deletion (step 28): .remove / .removeAt ---
check_prog "map-remove"    'let m={"a":1,"b":2,"c":3}; print m.remove("b"); print m.has("b"); print m.len();' "$(printf 'true\nfalse\n2')"
check_prog "map-remove-absent" 'let m = {"a":1}; print m.remove("z");' "false"
check_prog "map-remove-reuse" 'let m={"x":1}; m.remove("x"); m["x"]=99; print m["x"]; print m.len();' "$(printf '99\n1')"
check_prog "map-remove-probe" 'let m={}; for (let i in 0..20){ m[i]=i*i; } m.remove(5); m.remove(10); print m.has(15); print m[15]; print m.len();' "$(printf 'true\n225\n18')"
check_prog "arr-removeat"  'let a=[10,20,30,40]; print a.removeAt(1); print a;' "$(printf '20\n[10, 30, 40]')"
check_prog_err "arr-removeat-oob" 'let a=[1]; a.removeAt(5); print a;'

# --- integer ranges in for-in (step 27) ---
check_prog "range-basic"   'for (let i in 0..4) print i;' "$(printf '0\n1\n2\n3')"
check_prog "range-sum"     'let s=0; for (let i in 1..101) { s+=i; } print s;' "5050"
check_prog "range-expr-bounds" 'let n=3; for (let i in n..n+n) print i;' "$(printf '3\n4\n5')"
check_prog "range-break-cont" 'for (let i in 0..10) { if (i==3) continue; if (i==6) break; print i; }' "$(printf '0\n1\n2\n4\n5')"
check_prog "range-empty"   'let c=0; for (let i in 5..5) { c+=1; } print c;' "0"
check_prog "range-nested"  'let s=0; for (let i in 0..3) { for (let j in 0..3) { s+=1; } } print s;' "9"

# --- break / continue (step 26) ---
check_prog "break-while"   'let i=0; while (true) { if (i==3) break; print i; i+=1; }' "$(printf '0\n1\n2')"
check_prog "continue-for"  'for (let i=0; i<6; i+=1) { if (i-(i/2)*2==0) continue; print i; }' "$(printf '1\n3\n5')"
check_prog "continue-terminates" 'let c=0; for (let i=0;i<50;i+=1){ c+=1; continue; } print c;' "50"
check_prog "break-forin"   'for (let x in [10,20,30,40]) { if (x==30) break; print x; }' "$(printf '10\n20')"
check_prog "continue-forin" 'let s=0; for (let x in [1,2,3,4,5]) { if (x==3) continue; s+=x; } print s;' "12"
check_prog "break-nested"  'for (let i in [1,2]) { for (let j in [1,2,3]) { if (j==2) break; print i*10+j; } }' "$(printf '11\n21')"
check_prog "break-locals"  'let i=0; while (i<9) { let a=i*2; if (a>5) break; print a; i+=1; } print "x";' "$(printf '0\n2\n4\nx')"
check_prog_err "break-outside"   'break;'
check_prog_err "continue-outside" 'continue;'
check_prog_err "break-cross-fn"  'while (true) { fn f() { break; } }'
check_native "nat-break" 'fn f(): int { let s: int = 0; let i: int = 0; while (i < 10) { if (i == 5) break; s += i; i += 1; } return s; } print f();' "10"

# --- error handling: try / catch / throw (step 25) ---
check_prog "try-basic"    'try { throw "boom"; print "unreached"; } catch (e) { print "caught: " + e; }' "caught: boom"
check_prog "try-no-throw" 'try { print "ok"; } catch (e) { print "no"; }' "ok"
check_prog "try-across-call" 'fn r(n) { if (n < 0) { throw "neg"; } return n*2; } try { print r(5); print r(-1); } catch (e) { print "err: " + e; }' "$(printf '10\nerr: neg')"
check_prog "try-nested"   'try { try { throw "in"; } catch (e) { throw "out:" + e; } } catch (e) { print e; }' "out:in"
check_prog "try-nonstr"   'try { throw 42; } catch (e) { print e + 1; }' "43"
check_prog "try-return"   'fn f(): int { try { return 9; } catch (e) { return -1; } } print f(); try { throw "x"; } catch (e) { print e; }' "$(printf '9\nx')"
check_prog "try-recover"  'fn safe(n) { try { if (n == 0) { throw "zero"; } return 100 / n; } catch (e) { return -1; } } print safe(4); print safe(0);' "$(printf '25\n-1')"
check_prog_err "throw-uncaught" 'throw "unhandled";'
check_native_err "nat-rej-try" 'fn f(): int { try { return 1; } catch (e) { return 2; } } print f();'

# --- native backend: compile to C -> a real executable (step 9) ---
# Each program is the typed first-order subset; its native output must match.
check_native "nat-arith"    'print 2 + 3 * 4;'                          "14"
check_native "nat-typed-var" 'let x: int = 10; print x * x;'            "100"
check_native "nat-bool"     'let b: bool = 1 < 2; print b;'             "true"
check_native "nat-str"      'let s: str = "hi"; print s + "!";'         "hi!"
check_native "nat-streq"    'print "ab" == "a" + "b";'                  "true"
check_native "nat-fn"       'fn add(a: int, b: int): int { return a+b; } print add(3,4);' "7"
check_native "nat-recur"    'fn f(n: int): int { if (n<=1) return 1; return n*f(n-1); } print f(6);' "720"
check_native "nat-mutual"   'fn ev(n: int): bool { if(n==0) return true; return od(n-1); } fn od(n: int): bool { if(n==0) return false; return ev(n-1); } print ev(8);' "true"
check_native "nat-while"    'let i: int = 0; while (i < 3) { print i; i = i + 1; }' "$(printf '0\n1\n2')"
check_native "nat-for"      'for (let i: int = 0; i < 3; i = i + 1) print i*i;' "$(printf '0\n1\n4')"
check_native "nat-ifelse"   'let n: int = 5; if (n > 3) print "big"; else print "small";' "big"
check_native "nat-infer-global" 'let x = 21; print x + x;'              "42"
check_native "nat-div0"     'let z: int = 0; print 6 / 1;'              "6"
# Out-of-subset features must be rejected by the native backend.
check_native_err "nat-rej-closure" 'fn mk(): int { let n: int = 0; fn inc(): int { return n; } return inc(); } print mk();'
check_native_err "nat-rej-dynparam" 'fn f(x) { return x; } print f(1);'
# Collection-typed values need the GC runtime, so the scalar native backend rejects them.
check_native_err "nat-rej-array"   'fn id(a: [int]): [int] { return a; } print 1;'
# Method calls dispatch through the runtime — also outside the native subset.
check_native_err "nat-rej-method"  'fn f(): int { return "x".len(); } print f();'
# Arrays are heap/GC values — outside the scalar native subset.
check_native_err "nat-rej-arrlit"  'let a = [1, 2, 3]; print a[0];'
# Maps likewise.
check_native_err "nat-rej-maplit"  'let m = {"a": 1}; print m["a"];'

rm -f "$tmp" "${tmp}.native" "${tmp}.native.c" 2>/dev/null
echo "-----------------------------------------"
echo "passed: $pass   failed: $fail"
[ "$fail" -eq 0 ]

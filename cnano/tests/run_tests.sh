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

# --- functions (step 6) ---
check_prog "fn-basic"     'fn add(a,b){return a+b;} print add(3,4);'           "7"
check_prog "fn-no-args"   'fn answer(){return 42;} print answer();'            "42"
check_prog "fn-void-nil"  'fn p(x){print x;} print p(5);'                      "$(printf '5\nnil')"
check_prog "fn-implicit-nil" 'fn f(){} print f();'                            "nil"
check_prog "fn-locals"    'fn poly(x){let s=x*x; return s+x+1;} print poly(5);' "31"
check_prog "fn-recursion" 'fn fact(n){if(n<=1)return 1; return n*fact(n-1);} print fact(5);' "120"
check_prog "fn-fib"       'fn fib(n){if(n<2)return n; return fib(n-1)+fib(n-2);} print fib(10);' "55"
check_prog "fn-mutual"    'fn ev(n){if(n==0)return true; return od(n-1);} fn od(n){if(n==0)return false; return ev(n-1);} print ev(8);' "true"
check_prog "fn-nested"    'fn outer(){fn inner(x){return x*2;} return inner(21);} print outer();' "42"
check_prog "fn-as-value"  'fn sq(x){return x*x;} let f = sq; print f(9);'      "81"
check_prog "fn-early-ret" 'fn f(x){if(x>0)return "pos"; return "nonpos";} print f(5); print f(-1);' "$(printf 'pos\nnonpos')"
check_prog "fn-bare-ret"  'fn f(){return;} print f();'                        "nil"
check_prog "fn-uses-global" 'let g=100; fn add(x){return x+g;} print add(1);'  "101"
check_prog "fn-arg-expr"  'fn id(x){return x;} print id(2+3*4);'               "14"

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
# A heavily-reused name is fine (constant dedup keeps it to one slot).
check_prog "dedup-reuse"    'let c = 0; c = c + 1; c = c + 1; c = c + 1; print c;' "3"

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
check "bi-abs"       'abs(-7)'                 "7"
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
check_native "nat-match" 'fn c(n: int): int { match (n) { 0 => return 100; 1 => return 200; _ => return 0; } } print c(1);' "200"

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

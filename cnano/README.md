# cnano

A tiny low-level programming language, built from scratch in C **to learn how
programming languages work**. This first slice compiles and runs integer
arithmetic through a complete, real-world compiler pipeline:

```
source text  →  Lexer  →  tokens  →  Parser  →  AST  →  Compiler  →  bytecode  →  VM  →  result
```

Every stage above is a separate, well-commented module — the same architecture
used by production language implementations like CPython, Lua, and the JVM.

## Quick start

```bash
make            # build  -> build/cnano
make test       # run the end-to-end test suite (138 cases)
make run        # start the REPL

# run a file
./build/cnano examples/closures.cn            #  counters, adders, an account
./build/cnano examples/functions.cn           #  recursion, fib, first-class fns

# see the bytecode AND a step-by-step VM trace (the best way to learn)
./build/cnano --dump examples/variables.cn

# REPL (statements end with ';'; output only via `print`)
./build/cnano
> let x = 21; print x * 2;
42
```

## What it supports today

- **Programs are sequences of statements**, each ending with `;`, run top to
  bottom. Output happens only via `print EXPR;`
- **Variables**: `let x = …;` to declare, `x` to read, `x = …` to reassign
  (assignment is a right-associative expression, so `print a = 5;` works and
  `a = b = 1;` chains)
- **Block scope** with `{ }` and **local variables**: `let` inside a block makes
  a *local*, resolved to a stack slot at compile time (no runtime lookup, unlike
  globals). Supports **shadowing**; flags redeclaration and self-referential
  initialisers at compile time
- **Control flow** (cnano is now Turing-complete): `if`/`else`, `while`, and
  `for` (which desugars to a block + while), built from jump instructions and
  backpatching
- **Short-circuiting** `and` / `or` that return the deciding operand (so
  `nil or "default"` yields `"default"` and the skipped side never runs)
- **Functions**: `fn name(params) { ... }`, `return`, calls with a real
  call-frame stack and calling convention. First-class (assignable to
  variables), support **recursion** and **mutual recursion**, with arity
  checking, a controlled **stack-overflow** error, and multi-frame **stack
  traces** on runtime errors
- **Closures**: nested functions **capture variables** from enclosing functions
  via upvalues, and those variables outlive the frame that created them (so a
  returned counter keeps counting). Captured variables can be shared and mutated
  between sibling closures
- **Strings**: `"double-quoted"` literals, `+` concatenates them, and they are
  **interned** so equal strings compare in O(1) by pointer
- **Line comments** with `//` (stripped by the lexer; `/` is still division)
- Four runtime types: **integers** (64-bit signed), **booleans**, **nil**, and
  heap **strings**, represented with a tagged union + an object header (see
  `value.h` / `object.h`)
- Integer arithmetic `+  -  *  /` with correct **precedence** and
  **left-associativity**, unary minus (`-5`, even `--5`)
- **Comparisons** `<  <=  >  >=` and **equality** `==  !=` (no implicit
  cross-type coercion: `1 == true` is `false`)
- **Logical not** `!` with a defined truthiness rule (`nil` and `false` are
  falsey; *every* integer including `0` is truthy)
- Parentheses for grouping
- **Runtime type checking**: arithmetic/ordering on non-integers (e.g.
  `1 + true`, `true < false`), reading/assigning an undefined variable, and
  mixing string/int with `+` are all clean runtime errors, not crashes
- **Multi-error parsing**: a syntax error doesn't stop the parse — it recovers at
  the next statement and reports further independent errors (panic-mode recovery)
- Controlled errors: syntax errors, unexpected characters, **division by zero**,
  and type errors are all reported with a line number instead of crashing

## The files, in reading order

| File | Role | Compiler concept |
|------|------|------------------|
| `src/common.h` | shared toolbox | naming the concept vs. the representation |
| `src/lexer.{h,c}` | text → tokens | lexing, string slices, lookahead, comments |
| `src/ast.{h,c}` | the tree + `Program` | ASTs, tagged unions, expr vs. statement |
| `src/parser.{h,c}` | tokens → AST | recursive descent, precedence, l-values, recovery |
| `src/value.{h,c}` | values + constant pool | tagged-union dynamic values |
| `src/object.{h,c}` | heap objects: strings, functions, closures, upvalues | object model, interning |
| `src/table.{h,c}` | hash table | open addressing, linear probing, tombstones |
| `src/chunk.{h,c}` | bytecode container | designing an instruction set (ISA) |
| `src/compiler.{h,c}` | AST → per-function bytecode | scopes, slots, jumps, upvalue resolution |
| `src/vm.{h,c}` | executes bytecode | call frames; upvalue capture/closing; type checks |
| `src/debug.{h,c}` | disassembler | seeing what your compiler produced |
| `src/main.c` | CLI / REPL | wiring it together |

## Learning path

Read **[`docs/GUIDE.md`](docs/GUIDE.md)** — it walks through every stage, the
design trade-offs behind each decision, and a roadmap of what to build next
(variables, statements, control flow, functions, a type system, native code
generation). It is written to be read top-to-bottom alongside the source.

## Why C, why a stack VM, why bytecode?

These are deliberate, explained trade-offs — see the guide — but in short:
**C** puts you face-to-face with memory and pointers (the point of a low-level
project); a **stack VM** is the easiest target to compile to and to implement;
and **bytecode** (rather than a direct tree-walking interpreter) is the design
that real, fast languages use, so you learn the realistic architecture.

> This project lives in a subdirectory of another repo for convenience. It is
> fully self-contained — nothing here depends on the parent project. To extract
> it into its own git repository, see the bottom of `docs/GUIDE.md`.

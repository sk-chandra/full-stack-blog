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
make test       # run the end-to-end test suite (39 cases)
make run        # start the REPL

# run a file
./build/cnano examples/arithmetic.cn          #  prints 4
./build/cnano examples/booleans.cn            #  prints true

# see the bytecode AND a step-by-step VM trace (the best way to learn)
./build/cnano --dump examples/arithmetic.cn

# REPL
./build/cnano
> (1 + 2) * 3
9
```

## What it supports today

- Three runtime types: **integers** (64-bit signed), **booleans**, and **nil**,
  represented with a tagged union (see `value.h`)
- Integer arithmetic `+  -  *  /` with correct **precedence** and
  **left-associativity**, unary minus (`-5`, even `--5`)
- **Comparisons** `<  <=  >  >=` and **equality** `==  !=` (no implicit
  cross-type coercion: `1 == true` is `false`)
- **Logical not** `!` with a defined truthiness rule (`nil` and `false` are
  falsey; *every* integer including `0` is truthy)
- Parentheses for grouping
- **Runtime type checking**: arithmetic/ordering on non-integers (e.g.
  `1 + true`, `true < false`) is a clean runtime error, not a crash
- Controlled errors: syntax errors, unexpected characters, **division by zero**,
  and type errors are all reported with a line number instead of crashing

## The files, in reading order

| File | Role | Compiler concept |
|------|------|------------------|
| `src/common.h` | shared types (`Value`) | naming the concept vs. the representation |
| `src/lexer.{h,c}` | text → tokens | lexing / tokenizing, string slices |
| `src/ast.{h,c}` | the tree | abstract syntax trees, tagged unions |
| `src/parser.{h,c}` | tokens → AST | recursive descent, precedence, associativity |
| `src/value.{h,c}` | constant pool | why literals live in a side table |
| `src/chunk.{h,c}` | bytecode container | designing an instruction set (ISA) |
| `src/compiler.{h,c}` | AST → bytecode | tree walk → stack-machine code |
| `src/vm.{h,c}` | executes bytecode | the fetch-decode-execute loop |
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

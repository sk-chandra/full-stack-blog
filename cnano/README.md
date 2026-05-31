# cnano

A small programming language, built from scratch in C **to learn how programming
languages work** — from integer arithmetic up through closures and an optional
static type checker, via a complete, real-world compiler pipeline:

```
source  →  Lexer  →  Parser  →  AST  →  Type checker  →  Optimiser  →  Compiler  →  bytecode  →  VM  →  result
```

Every stage above is a separate, well-commented module — the same architecture
used by production language implementations like CPython, Lua, and the JVM.

## Quick start

```bash
make            # build  -> build/cnano
make test       # run the end-to-end test suite (524 cases, incl. native + GC)
make gcstress   # run the suite collecting on every allocation, under ASan
make run        # start the REPL

# run a file
./build/cnano examples/types.cn               #  optional static type annotations
./build/cnano examples/closures.cn            #  counters, adders, an account
./build/cnano examples/arrays.cn              #  arrays: literals, indexing, methods
./build/cnano examples/maps.cn                #  maps: any-key dictionaries, methods
./build/cnano examples/structs.cn             #  structs: records, fields, methods
./build/cnano examples/errors.cn              #  try / catch / throw
./build/cnano examples/showcase.cn            #  floats, unions, match, methods, ...

# see the bytecode AND a step-by-step VM trace (the best way to learn)
./build/cnano --dump examples/variables.cn

# compile the typed subset to a NATIVE executable (no interpreter), then run it
./build/cnano --native examples/native.cn -o /tmp/demo && /tmp/demo
./build/cnano --emit-c examples/native.cn      # or just inspect the generated C

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
  `a = b = 1;` chains), plus **compound assignment** `+= -= *= /= %=` on
  variables and index targets (`a[i] += 1`, `m["k"] += 1`). **`const`** declares
  an immutable binding (reassignment is a compile error; the contents of a const
  array/map are still mutable)
- **Block scope** with `{ }` and **local variables**: `let` inside a block makes
  a *local*, resolved to a stack slot at compile time (no runtime lookup, unlike
  globals). Supports **shadowing**; flags redeclaration and self-referential
  initialisers at compile time
- **Control flow** (cnano is now Turing-complete): `if`/`else`, `while`, C-style
  `for`, **`for (let x in coll)`** iteration over arrays (elements) and maps
  (keys), and **integer ranges** `for (let i in 0..n)` (end-exclusive, bound
  evaluated once) — all desugared to a block + while, built from jump
  instructions and backpatching — plus **`break`** and **`continue`** (which
  correctly runs a `for`/range step rather than skipping it, and discards
  loop-body locals)
- **`match`** value *and* type dispatch: `match (x) { 0 => …; "go" => …; _ => … }`
  or `match (v) { is int => …; is str => …; _ => … }` — desugars to an
  evaluate-once if/else-if chain (over `==` for value arms, `is` for type arms),
  with `_` as the default. A type arm **narrows** the matched variable inside its
  body, so `is int => return v * 2` typechecks `v` as `int`
- **Short-circuiting** `and` / `or` that return the deciding operand (so
  `nil or "default"` yields `"default"` and the skipped side never runs)
- **Conditional expression** `cond ? a : b` — a value-producing `if`, compiled
  from jumps so only the taken branch runs. Right-associative, so
  `n > 0 ? "pos" : n < 0 ? "neg" : "zero"` chains; works in the native backend
  too (lowers to C's own `?:` when both branches share a scalar type)
- **Error handling**: `throw EXPR;` raises any value; `try { … } catch (e) { … }`
  recovers from it. A throw **unwinds the call stack** (closing open upvalues) to
  the nearest enclosing `catch`; uncaught, it aborts with the value. Handlers are
  correctly discarded when a function returns out of a `try`
- **Functions**: `fn name(params) { ... }`, `return`, calls with a real
  call-frame stack and calling convention. First-class (assignable to
  variables), support **recursion** and **mutual recursion**, with arity
  checking, a controlled **stack-overflow** error, and multi-frame **stack
  traces** on runtime errors
- **Closures**: nested functions **capture variables** from enclosing functions
  via upvalues, and those variables outlive the frame that created them (so a
  returned counter keeps counting). Captured variables can be shared and mutated
  between sibling closures
- **Builtins & methods**: native functions implemented in C — `clock()`,
  `str(x)`, `len(x)`, `type(x)`, `assert(c)`, math (`abs min max sqrt floor ceil round pow`) — registered as
  globals, plus **method-call syntax** `receiver.method(args)` that dispatches on
  the receiver's type via a fused `OP_INVOKE`. A small standard library: strings
  have `.len/.upper/.lower/.contains/.indexOf/.substring/.trim/.startsWith/
  .endsWith/.replace/.repeat/.split`; arrays add `.contains/.indexOf/.join/.sort/
  .reverse/.slice` plus **higher-order** `.map/.filter/.reduce` (which call a
  cnano function back from C — the VM is re-entrant); maps add `.values`. `.split`
  and `.join` round-trip; `.reverse`/`.slice` copy (leaving the receiver intact)
- **Arrays**: `[1, 2, 3]` literals, indexing `a[i]` and `a[i] = v`
  (bounds-checked), nesting (`m[1][0]`), and methods `.len()`/`.push(x)`/`.pop()`.
  A heap object **managed by the GC** (elements are traced). Typed as `[T]` and
  checked structurally, or `[any]` to stay fully dynamic/heterogeneous. Also
  `.removeAt(i)`, `.reverse()` and `.slice(start, end)`
- **Maps**: `{"a": 1, "b": 2}` literals with **any hashable key** (int, bool,
  nil, str — its own value-keyed hash table with **tombstone**-based deletion),
  `m[k]` get/set sharing the array index opcodes, and methods
  `.len()`/`.has(k)`/`.keys()`/`.values()`/`.remove(k)`. GC-traced keys *and*
  values; typed as `{K: V}` and checked structurally
- **Structs**: `struct Point { x: int, y: int }` declares a record type;
  `Point(1, 2)` constructs an instance, `p.x` / `p.x = v` access fields (the same
  `.` that calls methods). **Methods** live in the struct body and receive the
  instance as `self` (`fn area(): int { return self.w * self.h; }`); they can
  mutate fields and call each other. GC-managed instances. Statically **nominal**
  (`A` ≠ `B` even with identical fields), with construction arg/arity, field-type,
  and unknown-field/type errors caught before execution (method bodies too);
  usable as `[Point]`, `{str: Point}`, function params/returns, etc.
- **Optional static types** (gradual typing): annotate with `let x: int = …;`
  and `fn add(a: int, b: int): int { … }`. Types are **structured** —
  `int`/`bool`/`str`/`nil`/`any` plus the parametric `[T]` (arrays) and
  `{K: V}` (maps), nesting arbitrarily (`[[int]]`, `{str: [int]}`). A
  type-checking pass runs **before** execution and rejects mismatches (bad
  initialisers, wrong argument types/arity, wrong return type, calling a
  non-function, bad operators); collection types are checked **structurally**
  (`[int]` ≠ `[bool]`, recursing into element/key/value). **Nullable** types
  `T?` mean "T or nil"; **union** types `int | str` mean "one of these". You
  can't use either where a specific type is required until you **narrow** it —
  with `if (x != nil) { … }` for nullables or `if (x is int) { … }` for unions
  (the runtime `x is T` test doubles as the checker's narrowing guard, treating
  `x` as the tested type inside the branch). Unannotated code is `any` and stays
  fully dynamic, so typed and untyped code mix freely
- **Modules**: `import "path.cn";` at the top level pulls another file's
  declarations into the program. Paths resolve **relative to the importing file**
  (so a library's own imports work no matter who imports it), and each file is
  included **at most once**, so diamonds and cycles are safe. Imports are resolved
  by a pass that splices every file into one program *before* type-checking, so a
  type error anywhere — across file boundaries — is still caught up front
- **Optimisation**: an AST **constant-folding** pass evaluates constant
  subexpressions at compile time (`2 + 3 * 4` → `14`, `"a" + "b"` → `"ab"`),
  constant **deduplication**, and an `OP_CONSTANT_LONG` form so chunks aren't
  capped at 256 constants
- **Native compilation** (ahead-of-time): `cnano --native file.cn -o prog`
  compiles the **statically-typed, first-order subset** to C and invokes the
  system `cc`, producing a standalone native executable with no interpreter.
  Types are known, so the emitted C is **unboxed** (`int64_t`/`double`/`bool`/
  `const char*`) — genuinely fast. floats lower to C `double` and the numeric
  built-ins (`sqrt`/`floor`/`ceil`/`round`/`pow`/`abs`/`min`/`max`) lower onto
  `<math.h>`, byte-for-byte matching the VM. `--emit-c` prints the generated C.
  The VM still runs the full dynamic language; anything outside the scalar
  subset (collections, structs, closures, nullable/union, `nil`) is cleanly
  rejected rather than miscompiled
- **Garbage collection**: a **mark-and-sweep** tracing collector reclaims dead
  heap objects *while the program runs* (an allocation-churning loop stays at
  bounded memory instead of growing forever). Tri-colour marking with an explicit
  grey worklist, a self-tuning heap-growth threshold, and a **weak** string-intern
  table. `make gcstress` runs the whole suite collecting on *every* allocation
  under ASan — the torture test for missed roots
- **Strings**: `"double-quoted"` literals, `+` concatenates them, and they are
  **interned** so equal strings compare in O(1) by pointer. **Interpolation**
  `"x = ${expr}"` embeds any expression (auto-converted with `str()`); the lexer
  is interpolation-aware so nested quotes and nested `${…}` work. **Escape
  sequences** `\n \t \r \" \\` (and `\$` to escape interpolation), correctly
  re-escaped in the native backend too
- **Line comments** with `//` (stripped by the lexer; `/` is still division)
- Runtime types: **integers** (64-bit signed), **floats** (64-bit IEEE double,
  written `3.14`), **booleans**, **nil**, and heap objects — a tagged union + an
  object header (see `value.h` / `object.h`)
- Arithmetic `+  -  *  /` works on ints *and* floats with **int→float promotion**
  (`5 + 2.5` → `7.5`, `10/4` → `2` but `10.0/4` → `2.5`); `%` and **bitwise**
  `&  |  ^  <<  >>  ~` are integers-only. Numeric `==` is cross-type (`3 == 3.0`).
  Correct C-style **precedence** and **left-associativity**, unary minus
  (`-5`, even `--5`)
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
| `src/module.{h,c}` | import resolution | splice multi-file programs into one (once-only) |
| `src/type.{h,c}` | the type system | gradual + structured types (`[T]`, `{K:V}`); arena-owned |
| `src/typecheck.{h,c}` | static analysis pass | tree-walking checker, two-pass for fns |
| `src/optimize.{h,c}` | AST optimisation pass | constant folding (bottom-up rewrite) |
| `src/codegen_c.{h,c}` | native backend | AST → C source → `cc` → executable (AOT) |
| `src/memory.{h,c}` | GC + allocator | mark-and-sweep, tri-colour worklist, weak intern table |
| `src/value.{h,c}` | values + constant pool | tagged-union dynamic values |
| `src/object.{h,c}` | heap objects: strings, functions, natives, arrays, maps, closures, upvalues | object model, interning, value-keyed map table |
| `src/builtins.{h,c}` | native functions + method dispatch | `clock`/`str`; `OP_INVOKE` method tables |
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

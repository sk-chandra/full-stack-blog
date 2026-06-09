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
make test       # run the end-to-end test suite (769 cases, incl. native + GC)
make fuzz       # generate hostile inputs and assert the compiler never crashes
make gcstress   # run the suite collecting on every allocation, under ASan
make bench      # run the self-timing benchmark suite
make run        # start the REPL

# run a file
./build/cnano examples/types.cn               #  optional static type annotations
./build/cnano examples/generics.cn            #  generics: fn id<T>(x: T): T (type-erased)
./build/cnano examples/prelude.cn             #  the std prelude — a library written IN cnano
./build/cnano examples/closures.cn            #  counters, adders, an account
./build/cnano examples/arrays.cn              #  arrays: literals, indexing, methods
./build/cnano examples/maps.cn                #  maps: any-key dictionaries, methods
./build/cnano examples/structs.cn             #  structs: records, fields, methods
./build/cnano examples/enums.cn               #  enums: named constants + match
./build/cnano examples/adt.cn                 #  tagged-union ADTs (struct+union+match)
./build/cnano examples/generators.cn          #  generators: yield / lazy sequences
./build/cnano examples/errors.cn              #  try / catch / throw
./build/cnano examples/showcase.cn            #  floats, unions, match, methods, ...

# see the bytecode AND a step-by-step VM trace (the best way to learn)
./build/cnano --dump examples/variables.cn
./build/cnano --debug examples/closures.cn     # stepping debugger: s/n/c, b LINE, p NAME, bt
./build/cnano --cfg examples/control_flow.cn   # control-flow graph (basic blocks)
./build/cnano --ir examples/ir.cn              # three-address IR, before + after optimisation
./build/cnano --types examples/showcase.cn     # inferred type of each top-level binding
./build/cnano --stats examples/showcase.cn     # profile: opcode/alloc/GC summary

# compile the typed subset to a NATIVE executable (no interpreter), then run it
./build/cnano --native examples/native.cn -o /tmp/demo && /tmp/demo
./build/cnano --emit-c examples/native.cn      # or just inspect the generated C
./build/cnano --asm examples/asm.cn            # x86-64 asm from the IR (regalloc + instr selection)
./build/cnano --elf examples/elf.cn -o /tmp/e && /tmp/e   # ...or a native ELF binary made
                                               # with NO cc/as/ld/libc at all (int subset)

# REPL (statements end with ';'; output only via `print`)
./build/cnano
> let x = 21; print x * 2;
42
```

## What it supports today

- **Programs are sequences of statements**, each ending with `;`, run top to
  bottom. Output happens only via `print EXPR;`
- **Number literals**: decimals and floats (`42`, `3.14`), plus `0xFF`
  hexadecimal, `0b1010` binary, `0o17` octal, and `_` digit separators
  (`1_000_000`, `0xFF_FF`). A leading-zero literal like `017` is decimal `17`
  (no surprise C-style octal)
- **Variables**: `let x = …;` to declare, `x` to read, `x = …` to reassign
  (assignment is a right-associative expression, so `print a = 5;` works and
  `a = b = 1;` chains), plus **compound assignment** `+= -= *= /= %= &= |= ^=
  <<= >>=` on variables and index targets (`a[i] += 1`, `m["k"] += 1`).
  **`const`** declares
  an immutable binding (reassignment is a compile error; the contents of a const
  array/map are still mutable)
- **Block scope** with `{ }` and **local variables**: `let` inside a block makes
  a *local*, resolved to a stack slot at compile time (no runtime lookup, unlike
  globals). Supports **shadowing**; flags redeclaration and self-referential
  initialisers at compile time
- **Control flow** (cnano is now Turing-complete): `if`/`else`, `while`,
  **`do { … } while (c);`** (body runs at least once), C-style
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
  body, so `is int => return v * 2` typechecks `v` as `int`. Matching an **enum**,
  **bool**, or **union** variable is checked for **exhaustiveness**: omit a case
  without `_` and it's a compile error (`non-exhaustive match on Color: missing
  Color.Blue`); a redundant `_` on an already-total match warns
- **Tagged-union ADTs** by composition: a variant is a `struct` (product type),
  the sum is a `union` (`Circle | Rect | Dot`), and you take them apart with an
  **exhaustive** `match` over `is` arms (whose narrowing exposes each variant's
  fields). No bespoke syntax — `examples/adt.cn` builds shapes and a recursive
  expression tree this way
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
  traces** on runtime errors. **Tail calls are optimised**: a `return f(args)`
  reuses the current frame (for closure callees), so deep/mutual tail recursion
  runs in O(1) stack — `count(1_000_000, 0)` returns instead of overflowing
  (suppressed inside a `try`, so a `catch` still sees the callee's throws)
- **Closures**: nested functions **capture variables** from enclosing functions
  via upvalues, and those variables outlive the frame that created them (so a
  returned counter keeps counting). Captured variables can be shared and mutated
  between sibling closures
- **Generators**: a function containing `yield` returns a **coroutine** — calling
  it freezes at the start; each `.next()` runs to the next `yield` (handing back
  its value) and `.done()` reports completion. Implemented by saving/restoring the
  generator's stack window + instruction pointer, so local state persists across
  yields, generators can be infinite (`fibs()`), and two from one function keep
  independent state. `examples/generators.cn`
- **Anonymous functions (lambdas)**: `fn(x) { … }` or the `fn(x) => expr`
  shorthand in any expression position — `[1,2,3].map(fn(x) => x*x)`. They are
  ordinary closures (capture, share, and outlive their scope just like named
  functions) and, when annotated, are arity/return-type checked at call sites
- **Builtins & methods**: native functions implemented in C — `clock()`,
  `str(x)`, `len(x)`, `type(x)`, `assert(c)`, math (`abs min max sqrt floor ceil
  round pow`), and text/number helpers (`parseInt`, `parseFloat`, `ord`, `chr`) —
  registered as globals, plus **method-call syntax** `receiver.method(args)` that
  dispatches on the receiver's type via a fused `OP_INVOKE`. A small standard
  library: strings have `.len/.upper/.lower/.contains/.indexOf/.substring/.trim/
  .startsWith/.endsWith/.replace/.repeat/.split`; arrays add `.contains/.indexOf/
  .join/.sort/.reverse/.slice/.sum/.min/.max` plus **higher-order**
  `.map/.filter/.reduce` (which call a cnano function back from C — the VM is
  re-entrant); maps add `.values`. `.split` and `.join` round-trip;
  `.reverse`/`.slice` copy (leaving the receiver intact)
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
- **Enums**: `enum Color { Red, Green, Blue }` declares a **nominal** type whose
  values are named singleton members, accessed as `Color.Red` (the same `.` as
  fields). Members compare by identity (`Color.Red == Color.Red`), print
  qualified (`Color.Red`), report `type()` as their enum name, and pair naturally
  with `match`. A value of one enum never satisfies another, and `: Color`
  annotations are checked (unknown members and cross-enum mixing are compile
  errors)
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
  `x` as the tested type inside the branch). Unannotated *values* stay `any` and
  fully dynamic, so typed and untyped code mix freely — but a function written
  without a `: T` return annotation has its return type **inferred** from the body
  (joining each `return`, plus `nil` if it can fall off the end), so its callers
  still get real checking. `cnano --types file` prints the inferred type of every
  top-level binding
- **Generics** (parametric polymorphism): `fn id<T>(x: T): T { return x; }`. Type
  variables are solved per call site by **unifying** the parameter types against
  the argument types, so `id(42)` is typed `int` and `id("hi")` is typed `str` —
  and the solved type is enforced (assigning `id(42)` to a `bool` is an error).
  Because cnano's runtime values are already tagged, generics are pure
  compile-time checking with **type erasure** (zero runtime cost) — the worked
  contrast with a monomorphising compiler like C++/Rust
- **Modules**: `import "path.cn";` at the top level pulls another file's
  declarations into the program. Paths resolve **relative to the importing file**
  (so a library's own imports work no matter who imports it), and each file is
  included **at most once**, so diamonds and cycles are safe. Imports are resolved
  by a pass that splices every file into one program *before* type-checking, so a
  type error anywhere — across file boundaries — is still caught up front
- **A standard prelude written in cnano** (`std/prelude.cn`): the language is
  expressive enough to implement its own library — `range`, a comparator
  `sortBy` (insertion sort), Euclid's `gcd`, exponentiation-by-squaring `ipow`,
  `any`/`all`/`countIf` predicates, generic `first<T>`/`last<T>`, and string
  padding — all running on the same VM as your code. `import "std/prelude.cn";`
- **Optimisation**: an AST **constant-folding** pass evaluates constant
  subexpressions at compile time (`2 + 3 * 4` → `14`, `"a" + "b"` → `"ab"`) and
  collapses **constant-condition** `?:`/`and`/`or` to the branch that would run
  (side-effect-safe — the skipped branch never ran anyway), plus constant
  **deduplication** and an `OP_CONSTANT_LONG` form so chunks aren't capped at 256
  constants. A **bytecode peephole pass** then deletes provably-dead instruction
  pairs (e.g. push-then-pop) and **recomputes the jump offsets** that span each
  hole. Global reads use a **monomorphic inline cache** (each site caches the
  globals-table entry index + a generation token, skipping the hash probe on a
  hit — ~15–25% faster on global-heavy code). `make bench` runs a self-timing
  benchmark suite, and `cnano --stats file` prints an opcode/allocation/GC
  profile — *measure before you optimise*
- **Diagnostics**: syntax errors render the offending source line and underline
  the exact token with a caret, the way a real compiler does:
  ```
  [line 1] Error at ';': Expect a value or '('.
       1 | let x = 1 +;
         |            ^
  ```
  and a misspelt name gets a **"did you mean …?"** hint (edit-distance based) for
  unknown types, struct fields, enum members, and undefined globals — e.g.
  `undefined variable 'conut' (did you mean 'count'?)`. Across **`import`ed
  files**, errors name the file they came from (`[lib/math.cn:2] Type error: …`),
  including each frame of a runtime stack trace; single-file programs keep the
  familiar `[line N]`. The compiler also builds a **control-flow graph** of each
  function (`--cfg` prints basic blocks + edges), **warns on unreachable code** (a
  statement after a definite `return`/`break`/`continue`/`throw`), and runs
  **dead-block elimination** over the CFG (dropping blocks the VM can never reach,
  with the jump offsets recomputed)
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
- **x86-64 assembly backend**: `cnano --asm file.cn` compiles scalar code —
  **ints, bools and floats**, `if`/`while` **control flow**, and **functions**
  (parameters, `return`, recursion and mutual recursion) — through the
  three-address IR all the way to real machine code, doing the jobs a back end
  must: **instruction selection** (comparisons → `cmp`/`setcc`, branches →
  `jmp`/`jz`, float arithmetic → the SSE2 `addsd`/`ucomisd` family), **register
  allocation** by **linear scan** with spilling (int temps in the five
  callee-saved registers; float temps in memory — the ABI has no callee-saved
  xmm), and a module-wide **type-class analysis** (int/bool/float per temp,
  variable, and function return — flowing through calls — since machine code has
  no runtime tags; mixed-class values are rejected, never miscompiled). Functions
  follow the **System V calling convention** (`rdi`/`rsi`/… and `xmm0`/…, results
  in `rax`/`xmm0`), and `print` output matches the VM byte-for-byte — including
  `3.0`, `nan`/`inf`, and `true`/`false` — via an emitted format helper
- **Direct ELF emission**: `cnano --elf file.cn -o prog` produces a runnable
  Linux binary with **no assembler, no linker, and no libc**: cnano encodes the
  x86-64 machine **bytes** itself (REX/ModRM/rel32, with the same backpatching
  its bytecode compiler uses for jumps), writes the ELF header and the two
  `PT_LOAD` program headers itself, and prints through raw `write` syscalls —
  hand-rolled integer-to-decimal conversion included. Where `--asm` teaches
  instruction *selection* and register allocation, this teaches instruction
  *encoding* and what an executable file actually is (integer/bool subset)
- **A stepping debugger**: `cnano --debug file.cn` pauses on the first line and
  takes commands — `s`tep (into calls), `n`ext (over them), `c`ontinue, `b LINE`
  breakpoints, `p NAME`, `vars`, `bt`. Printing a local by name works because the
  compiler now emits **debug information** (a name → stack-slot table with
  bytecode live ranges — DWARF in miniature); without it, compiled code knows
  locals only as slot numbers
- **Garbage collection**: a **mark-and-sweep** tracing collector reclaims dead
  heap objects *while the program runs* (an allocation-churning loop stays at
  bounded memory instead of growing forever). Tri-colour marking with an explicit
  grey worklist, a self-tuning heap-growth threshold, and a **weak** string-intern
  table. `make gcstress` runs the whole suite collecting on *every* allocation
  under ASan — the torture test for missed roots. **Observable**:
  `CNANO_GC_TRACE=1` logs each cycle and `--stats` reports cycles / bytes
  reclaimed / pause time / peak heap. (Non-moving by design — the GUIDE explains
  why cnano's raw-pointer roots rule out a copying collector without a bigger
  redesign)
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
| `src/suggest.{h,c}` | "did you mean …?" | Levenshtein distance + a length-scaled threshold |
| `src/type.{h,c}` | the type system | gradual + structured types (`[T]`, `{K:V}`); arena-owned |
| `src/typecheck.{h,c}` | static analysis pass | tree-walking checker, two-pass for fns |
| `src/optimize.{h,c}` | AST optimisation pass | constant folding (bottom-up rewrite) |
| `src/peephole.{h,c}` | bytecode optimisation pass | delete dead pairs + remap jump offsets |
| `src/cfg.{h,c}` | control-flow graph | basic blocks + edges over bytecode (`--cfg`); reachability |
| `src/dce.{h,c}` | dead-block elimination | drop unreachable blocks; recompute spanning jumps |
| `src/ir.{h,c}` | three-address IR | named temporaries; the home for value optimisations (`--ir`) |
| `src/iropt.c` | IR optimiser | constant propagation + folding, CSE (value numbering), dead-temp elimination |
| `src/codegen_c.{h,c}` | native (C) backend | AST → C source → `cc` → executable (AOT) |
| `src/codegen_x64.{h,c}` | x86-64 backend | IR → assembly; linear-scan register allocation + spilling (`--asm`) |
| `src/tclass.{h,c}` | type-class analysis | int/bool/float per temp/var/return, module-wide fixpoint (shared by the native backends) |
| `src/codegen_elf.{h,c}` | direct ELF backend | IR → machine-code **bytes** → an ELF executable, with no cc/as/ld/libc (`--elf`) |
| `src/memory.{h,c}` | GC + allocator | mark-and-sweep, tri-colour worklist, weak intern table |
| `src/value.{h,c}` | values + constant pool | tagged-union dynamic values |
| `src/object.{h,c}` | heap objects: strings, functions, natives, arrays, maps, closures, upvalues | object model, interning, value-keyed map table |
| `src/builtins.{h,c}` | native functions + method dispatch | `clock`/`str`; `OP_INVOKE` method tables |
| `src/table.{h,c}` | hash table | open addressing, linear probing, tombstones |
| `src/chunk.{h,c}` | bytecode container | designing an instruction set (ISA) |
| `src/compiler.{h,c}` | AST → per-function bytecode | scopes, slots, jumps, upvalue resolution |
| `src/vm.{h,c}` | executes bytecode | call frames; upvalue capture/closing; type checks |
| `src/debug.{h,c}` | disassembler | seeing what your compiler produced |
| `src/debugger.{h,c}` | stepping debugger | breakpoints, step into/over, `p NAME` via compiler-emitted debug info (`--debug`) |
| `src/main.c` | CLI / REPL | wiring it together |
| `tools/fuzzgen.c`, `tools/fuzz.sh` | fuzzer | generate hostile inputs; assert the compiler errors cleanly, never crashes (`make fuzz`, under ASan) |
| `std/prelude.cn` | the standard prelude | a library written **in cnano** — `range`, comparator `sortBy`, `gcd`, `ipow`, predicates, padding |

## Learning path

Read **[`docs/GUIDE.md`](docs/GUIDE.md)** — it walks through every stage, the
design trade-offs behind each decision, and a roadmap of what to build next
(variables, statements, control flow, functions, a type system, native code
generation). It is written to be read top-to-bottom alongside the source.

The language itself is specified in **[`docs/SPEC.md`](docs/SPEC.md)**: the full
EBNF grammar (lexical rules, every statement form, the 14-level expression
precedence ladder, the type grammar), the semantics highlights (truthiness,
promotion, narrowing), and the per-backend subset matrix.

## Why C, why a stack VM, why bytecode?

These are deliberate, explained trade-offs — see the guide — but in short:
**C** puts you face-to-face with memory and pointers (the point of a low-level
project); a **stack VM** is the easiest target to compile to and to implement;
and **bytecode** (rather than a direct tree-walking interpreter) is the design
that real, fast languages use, so you learn the realistic architecture.

> This project lives in a subdirectory of another repo for convenience. It is
> fully self-contained — nothing here depends on the parent project. To extract
> it into its own git repository, see the bottom of `docs/GUIDE.md`.

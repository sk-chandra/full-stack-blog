# Building cnano: A Guide to How Programming Languages Work

This guide teaches the ideas behind cnano by walking through the code in the
order data flows through it. Read it next to the source. By the end you will
understand, concretely, what happens between typing `(1 + 2) * 3` and seeing
`9` — and you will know the major design decisions and trade-offs that every
language implementer faces.

> **The mental model.** A language implementation is a *pipeline of
> translations*. Each stage takes a representation that is convenient for the
> previous stage and turns it into one that is convenient for the next. We start
> with text (convenient for humans) and end with instructions for a tiny
> software CPU (convenient for machines). Nothing magical happens; every step is
> a small, understandable transformation.

```
 text  →  Lexer  →  tokens  →  Parser  →  AST  →  Compiler  →  bytecode  →  VM  →  result
 "1+2"     scan      [1,+,2]   grammar   tree     tree walk    bytes        run     3
```

---

## 0. The big design decisions (and their trade-offs)

Before any code, a language designer makes a few high-level choices. Here are
the ones we made and *why*, because these are the questions you will face for
any language you build.

### Compiler vs. interpreter — and the middle path we chose

- A **tree-walking interpreter** executes the AST directly by recursively
  evaluating nodes. Easiest to write; **slowest** to run (pointer-chasing, a
  big `switch` per node, poor cache behaviour).
- A **native compiler** (like C → machine code) produces the fastest programs
  but is by far the most work: you must understand a real CPU's instruction set,
  registers, calling conventions, and an object-file format.
- **Bytecode + a virtual machine** (our choice) sits in between. We *compile* to
  a compact, made-up instruction set, then *interpret* those instructions in a
  tight loop. This is what CPython, Lua, Java, and C# all do. It is dramatically
  faster than tree-walking, far simpler than native codegen, and **portable** —
  the same bytecode runs anywhere the VM is compiled.

> **Trade-off in one sentence:** bytecode buys most of the speed of compilation
> for a small fraction of the complexity, at the cost of an interpretation
> overhead you can later remove with a JIT.

### Stack machine vs. register machine

Our VM is a **stack machine**: instructions implicitly operate on the top of an
operand stack (`OP_ADD` pops two, pushes one). The alternative is a **register
machine** (like Lua 5+), where instructions name their operands (`ADD r3, r1,
r2`).

- Stack machines produce **simpler compilers** (no register allocation) and
  **denser code** (no operand fields), which is why we use one.
- Register machines execute **fewer instructions** (less push/pop traffic) and
  are often faster, at the cost of a more complex compiler.

### Static vs. dynamic typing; AST vs. single-pass

- We build an **explicit AST** rather than emitting bytecode in one pass. A
  single pass is faster to compile and uses less memory, but an AST gives you a
  clean separation of concerns and a data structure you can *analyse* (type
  checking, optimisation, pretty-printing). For learning, the AST is the clear
  winner.
- Types: cnano currently has exactly one type (64-bit int), so there is no type
  checker yet. The roadmap below discusses adding one.

---

## 1. Values and memory: the foundation (`common.h`, `value.{h,c}`)

Everything starts with **what a value is**. In `common.h`:

```c
typedef int64_t Value;
```

We give the concept a *name* (`Value`) separate from its *representation*
(`int64_t`). This indirection is the first real language-design lesson: when you
later add floats, booleans, or heap objects, you change this one typedef (into a
tagged union) and a handful of helpers — not every line that touches a number.

**The constant pool** (`value.c`). Bytecode instructions are one byte. A literal
like `1000000` cannot fit in a byte. The universal solution: store literals once
in a side table (the *constant pool*) and let instructions carry a small *index*
into it. `ValueArray` is that table, implemented with the classic **dynamic
array** pattern (track `count` and `capacity`; double capacity when full for
amortised O(1) appends). You will see this exact pattern again in `Chunk`.

> **C lesson:** `realloc` grows a heap buffer, possibly moving it. We always
> assign the result back and check for `NULL`. Forgetting either is a top source
> of C bugs; `make debug` (AddressSanitizer) catches them.

---

## 2. The lexer: text → tokens (`lexer.{h,c}`)

The lexer reads characters and groups them into **tokens** — the smallest
meaningful units. After lexing, `1 + 2` is three tokens: `NUMBER(1)`, `PLUS`,
`NUMBER(2)`. The parser can now reason about token *kinds* instead of raw
characters.

Key ideas in the code:

- **String slices, not copies.** A `Token` stores a `start` pointer into the
  original source plus a `length`. Zero allocation per token; the source buffer
  must simply outlive the tokens (it does). This is a common performance idiom.
- **One token of lookahead.** `peek()` inspects the current character without
  consuming it; `advance()` consumes. Almost all hand-written lexers/parsers run
  on a small, fixed amount of lookahead.
- **Whitespace and lines.** `skipWhitespace()` discards spaces but counts `\n`
  so every token carries a line number for good error messages. (Comments, when
  you add them, are skipped in this same spot.)
- **Error tokens.** Instead of a separate error channel, an unexpected character
  produces a `TOKEN_ERROR` whose "text" is a message. The parser turns that into
  a reported error. Unifying the happy and error paths keeps the cursor logic
  simple.

---

## 3. The AST: structure as a tree (`ast.{h,c}`)

The parser's output is an **Abstract Syntax Tree**. "Abstract" because it throws
away surface details (whitespace, the parentheses themselves) and keeps only
structure. For `1 + 2 * 3`:

```
        (+)
       /   .
      1    (*)
          /   .
         2     3
```

The tree **encodes precedence as shape**: `*` sits below `+`, so it will be
evaluated first — automatically, because the compiler walks children before
parents.

**Tagged unions** (`ast.h`). A `Node` is one of several shapes (number, unary,
binary). C represents "one of several" with a `type` tag plus a `union` of the
possible payloads. You read the tag to know which union arm is valid. This is
the same technique real VMs use to store dynamically typed values, so it is
worth getting comfortable with.

**Ownership** (`ast.c`). The parser allocates nodes with `malloc`; `freeNode`
walks the tree **post-order** (children before parent) to free it all without
dangling pointers. In C, *someone* must own every allocation — being explicit
about it here previews how you would later manage heap objects at runtime.

---

## 4. The parser: tokens → AST (`parser.{h,c}`)

> The grammar shown here is the original arithmetic-only version, kept for
> teaching the core idea. It has since grown comparison/equality levels (§9) and
> a top statement layer (§10). The current full grammar lives in the header of
> `parser.c`.

cnano uses **recursive descent**: one function per grammar rule, and the C call
stack mirrors the nesting of the expression. The grammar (in the file header):

```
expression -> term ;
term        -> factor ( ( "+" | "-" ) factor )* ;   // left-associative
factor      -> unary  ( ( "*" | "/" ) unary  )* ;   // left-associative
unary       -> "-" unary | primary ;
primary     -> NUMBER | "(" expression ")" ;
```

Two beautiful things fall out of this structure for free:

- **Precedence = nesting order.** `term` calls `factor` calls `unary` calls
  `primary`. Because multiplication is parsed "deeper" than addition, `1 + 2 *
  3` groups the `*` first. To add a lower-precedence operator later (say
  comparison), you add a new rule *above* `term`.
- **Associativity = loop vs. recursion.** The `while` loops in `term`/`factor`
  fold left-to-right, so `10 - 2 - 3` becomes `(10 - 2) - 3`. If you wanted a
  *right*-associative operator (like exponentiation or assignment), you would
  recurse on the right instead of looping.

**Error handling.** The parser reports the first error with a line number and
sets `hadError`, then suppresses follow-on noise. `parse()` returns `NULL` on
failure and the caller skips compilation. (A production parser also
*synchronises* — skips to a safe point — to report multiple errors per run; a
good next exercise.)

> **Aside — Pratt parsing.** Recursive descent with one function per precedence
> level is clear but gets verbose with many levels. *Pratt parsing* (top-down
> operator precedence) collapses them into a single table-driven loop and is the
> technique used by many real compilers. Once you understand the version here,
> converting to a Pratt parser is an illuminating refactor.

---

## 5. The instruction set: designing the VM's language (`chunk.h`)

The `OpCode` enum **is** the instruction-set architecture (ISA) of our machine.
Designing it is designing the lowest-level language cnano actually runs:

```
OP_CONSTANT idx   push constants[idx]
OP_NEGATE         a = pop;            push -a
OP_ADD            b = pop; a = pop;   push a + b
OP_SUB / MUL / DIV   (same shape)
OP_RETURN         stop; result is top of stack
```

Design choices visible here:

- **One-byte opcodes, embedded operands.** Only `OP_CONSTANT` has an operand (a
  one-byte pool index). Compact, but the single byte caps a chunk at 256
  constants — a real ISA trade-off. The fix (an `OP_CONSTANT_LONG` with a wider
  operand) is noted in `compiler.c`.
- **A parallel `lines[]` array.** `lines[i]` records which source line produced
  `code[i]`, so runtime errors can name a line. Storing this as a parallel array
  is simple; real VMs compress it (run-length encoding) since consecutive
  instructions usually share a line.

---

## 6. The compiler: AST → bytecode (`compiler.{h,c}`)

The compiler does a **post-order walk** of the tree. To compile `a + b`:

1. compile `a`  → leaves `a`'s value on the stack
2. compile `b`  → leaves `b`'s value on the stack
3. emit `OP_ADD` → pops both, pushes the sum

That ordering is *exactly* what a stack machine consumes, which is the deep
reason trees map so cleanly onto stack bytecode. Notice we emit the left operand
before the right so that, at runtime, the right operand is on top — important
for non-commutative ops where `OP_SUB` must compute `a - b`, not `b - a`.

Every chunk ends with `OP_RETURN`, giving the VM an unambiguous stop signal and
a place to hand back the final value.

---

## 7. The virtual machine: execution (`vm.{h,c}`)

The VM is a **software CPU** running a **fetch-decode-execute** loop — the same
cycle silicon runs:

- **fetch:** read the byte at the instruction pointer (`ip`), advance it.
- **decode:** `switch` on the opcode.
- **execute:** manipulate the operand stack.

State is just two things: `ip` (which byte is next) and a fixed operand `stack`
with a `stackTop` pointer. `push`/`pop` move `stackTop`; there is no need to
clear popped memory — anything below `stackTop` is simply "not there".

Things worth studying:

- **The `BINARY_OP` macro.** `+`, `-`, `*`, `/` share the identical
  pop/pop/push shape, so a macro factors it out and the loop reads like an
  instruction manual. (Macros are a sharp tool; this is a classic, contained use
  of one.)
- **Turning hardware faults into language errors.** Integer division by zero is
  *undefined behaviour* in C and would crash the whole interpreter. `OP_DIV`
  checks for a zero divisor and raises a controlled `runtimeError` with a line
  number instead. A real language must never let user code crash the runtime —
  this is a central robustness lesson.
- **Tracing.** Run with `--dump` to print the stack before each instruction.
  Watching the stack rise and fall is the fastest way to build a correct mental
  model of stack-machine execution; an example trace is in the README.

---

## 8. Putting it together (`main.c`) and testing (`tests/`)

`main.c` is intentionally thin: it reads a file or runs a REPL, then calls
`interpret()`. Keeping the entry point small means the same core could be driven
by a different front end (an editor plugin, a web playground) without change. It
also maps interpreter outcomes onto conventional Unix exit codes (`65`
data/compile error, `70` runtime error) so cnano composes in shell pipelines.

`tests/run_tests.sh` is **golden/end-to-end testing**: feed an expression, check
the printed output (or, for error cases, a non-zero exit). We assert on
*observable behaviour*, which is what a language user actually depends on. Add a
line to that file for every new feature.

---

## 9. Case study: adding a second type (booleans, nil, comparisons)

This is roadmap step 1, completed. It is worth its own section because going from
*one* type to *two* is the single change that turns "a calculator" into "a
language". Here is everything it touched and why — a template for how a single
feature ripples through every stage of a compiler.

### 9.1 The representation: from `int64_t` to a tagged union

Before, `Value` was literally `int64_t`. With more than one type, a value must
say *what it is*, so `value.h` now holds a **tagged union**:

```c
typedef struct {
  ValueType type;            // VAL_NIL | VAL_BOOL | VAL_INT  — the tag
  union { bool boolean; int64_t integer; } as;  // the payload
} Value;
```

The union is only as big as its largest member, so booleans and ints share
storage; `type` says which member is live. Reading the wrong member is a bug, so
we funnel all access through macros — constructors (`INT_VAL`, `BOOL_VAL`,
`NIL_VAL`), predicates (`IS_INT`, …), and accessors (`AS_INT`, `AS_BOOL`). This
is *exactly* how CPython, Lua, and Ruby store dynamically typed values; you have
now built the core of a dynamic value system.

> **Trade-off:** a tagged struct is simple and debuggable but "fat" (16 bytes
> here, vs 8 for a bare int). Production VMs often shrink this with *NaN-boxing*
> (hiding a type tag inside the unused bits of a 64-bit IEEE float) — a great
> advanced exercise once this version makes sense.

### 9.2 The ripple through every stage

Adding the type forced a coordinated change across the whole pipeline — this
fan-out is the lesson:

| Stage | What changed |
|-------|--------------|
| **lexer** | new tokens: `!`, `!=`, `==`, `<`, `<=`, `>`, `>=`, and the keywords `true`/`false`/`nil`. Introduced two-char-operator lexing (`match('=')`) and keyword recognition (`identifierType`). |
| **AST** | new leaf nodes `NODE_INT`/`NODE_BOOL`/`NODE_NIL`; new ops `NOT`, `EQUAL`, `LESS`, `GREATER`. |
| **parser** | two new precedence levels (`equality`, `comparison`) above arithmetic; `!` in `unary`; the literals in `primary`. |
| **ISA** | new opcodes `OP_NIL/TRUE/FALSE/NOT/EQUAL/LESS/GREATER`. |
| **compiler** | emit the new nodes; wrap raw ints into `INT_VAL`. |
| **VM** | a `peek()`, runtime type checks, `valuesEqual`, the truthiness rule, and a varargs `runtimeError`. |

When people say a language feature is "cross-cutting," *this* is what they mean.

### 9.3 Three design decisions worth internalising

- **Keep the core small with desugaring.** The grammar offers `!=`, `<=`, `>=`,
  but the AST/compiler/VM only know `==`, `<`, `>`, and `!`. The parser
  *rewrites* the others (`a <= b` → `!(a > b)`), so three stages never grow code
  for them. Run `./build/cnano --dump` on `1 <= 2` and you will literally see
  `OP_GREATER` followed by `OP_NOT`. Real compilers desugar aggressively
  (for-loops → while-loops, `+=` → `=` plus `+`, and so on).

- **Truthiness is a choice, not a fact.** `isFalsey` in `vm.c` decides what
  counts as false. cnano follows Ruby/Lua: only `nil` and `false` are falsey, so
  `!0` is `false` (0 is truthy). C and Python would say 0 is falsey. There is no
  universally correct answer — but you must *pick one and apply it everywhere*.

- **No implicit coercion.** `1 == true` is `false`, not an error and not `true`.
  `valuesEqual` returns false for differing types rather than converting. This
  keeps ints and bools as genuinely distinct types — the opposite of
  JavaScript's `==`. It is the first whiff of a *type system*.

### 9.4 Runtime type checking — the robustness core

A typed value system is useless unless the VM *enforces* it. Arithmetic and
ordering now check `IS_INT` on both operands **before** unwrapping (`BINARY_OP`
and `OP_NEGATE`/`OP_DIV`), and raise a clean `runtimeError` otherwise. `OP_EQUAL`
and `OP_NOT`, by contrast, accept any type — a deliberate asymmetry. The
guarantee this buys: **no cnano program can crash the interpreter via a type
error or a divide-by-zero.** Turning would-be undefined behaviour into reported
errors is the difference between a toy and a usable tool.

---

## 10. Case study: statements, `print`, and the expression/statement split

This is roadmap step 2, completed. It is where cnano stops being "evaluate one
expression" and becomes "run a program" — a list of statements executed in
order. The headline idea is the **expression vs. statement distinction**, the
most important structural concept in language design.

### 10.1 Expression vs. statement — values vs. effects

- An **expression** *computes a value*: `1 + 2`, `a < b`, `true`. In our VM,
  compiling an expression always leaves exactly **one** value on the stack.
- A **statement** *performs an action* and yields **no** value: `print x;`, or a
  bare `expr;` evaluated only for its (future) side effects. In our VM, a
  statement is **stack-neutral** — it ends with the stack at the height it
  started.

Those two contracts (one in `emitExpr`, the opposite in `emitStatement` in
`compiler.c`) are the backbone of the whole step. Keeping them crisp is what
makes everything that follows — variables, blocks, control flow — compose
cleanly.

### 10.2 `OP_POP`: why expression statements must clean up

Consider `1 + 2;` — an expression statement. The expression pushes `3`, but the
statement uses nothing. If we left `3` on the stack, every such statement would
leak one slot and the stack would grow without bound. So `emitStatement` emits an
**`OP_POP`** after the expression to discard its value. This is the concrete
reason a stack VM needs a pop instruction at all, and forgetting it is a classic
bug. `print x;` is the same shape but pops *via* `OP_PRINT` (which consumes its
operand), so it too nets to zero. Run `--dump` on `1+2; print 7;` and you'll see
`OP_ADD, OP_POP` then `OP_CONSTANT, OP_PRINT`.

### 10.3 Output becomes an explicit effect

Previously the REPL auto-printed the final value — output was a side effect of
the *host*, not the language. Now the only way a program produces output is
`print`, compiled to `OP_PRINT`. `OP_RETURN` no longer carries a result; it just
halts. This is a real design stance: a program's observable behaviour is the
effects it explicitly performs, not whatever happened to be left lying around.

### 10.4 Program structure and a new owner

The parser's entry point changed from "return one expression tree" to "fill a
`Program`" — a growable array of statement nodes (same dynamic-array pattern as
`Chunk`/`ValueArray`). `Program` *owns* its statements; `freeProgram` frees each
tree then the array. The grammar grew a new top layer:

```
program   -> statement* EOF ;
statement -> "print" expression ";" | expression ";" ;
```

Note how cleanly the new layer sits *above* the entire expression grammar from
steps 0–1 — we added structure without disturbing what already worked.

### 10.5 Better errors: panic-mode recovery

With multiple statements, stopping at the first syntax error is unfriendly. The
parser now enters **panic mode** on an error (suppressing cascade messages),
then `synchronize()` skips tokens until a statement boundary (just past a `;`, or
at a keyword like `print`) and resumes. One run can now report several
*independent* errors — exactly how real compilers behave. `hadError` (did
anything fail?) and `panicMode` (are we mid-recovery?) are deliberately separate
flags.

### 10.6 A free addition: line comments

Because the step needed example programs worth annotating, the lexer learned
`//` line comments. The lesson is *where* they live: comments are stripped inside
`skipWhitespace`, never becoming tokens, because they carry no meaning for the
parser. Distinguishing `//` from a single `/` (division) requires **two**
characters of lookahead (`peekNext`) — a small but real lexer technique.

---

## 11. Case study: global variables, a hash table, and heap objects

Roadmap step 3, completed — by far the largest step, and the one with the most
classic computer-science content. It introduces cnano's first heap-allocated
values and the data structure that stores variables: a hash table built from
scratch. Five intertwined ideas:

### 11.1 Heap objects and "inheritance" in C (`object.{h,c}`)

Integers, booleans and nil fit inside a `Value`. Strings don't — they are
variable-length, so they live on the heap and the `Value` only points at them
(the new `VAL_OBJ` tag). Every heap value shares a common header, `Obj`, and each
concrete type (so far just `ObjString`) puts an `Obj` as its **first field**:

```c
struct Obj { ObjType type; struct Obj *next; };
struct ObjString { Obj obj; int length; char *chars; uint32_t hash; };
```

Because `obj` is first, a `ObjString*` can be safely cast to `Obj*` and back —
this is **inheritance via struct embedding**, the standard C technique behind
this kind of object system. The `type` tag recovers the concrete type. All
objects are threaded onto a VM-owned intrusive linked list (`obj.next` →
`vm.objects`) at birth, so `freeObjects()` can reclaim them at shutdown. That
list is the exact hook a **garbage collector** would later use — we just free
everything manually for now.

### 11.2 A hash table from scratch (`table.{h,c}`)

The store behind both globals and string interning. Design choices, each a real
trade-off:

- **Open addressing + linear probing**, not separate chaining. All entries sit
  in one flat array; on collision we walk forward (wrapping) to the next slot.
  This is cache-friendly and allocation-free per entry (chaining pointer-chases
  and mallocs per node). The cost is the tombstone subtlety below.
- **Power-of-two capacity** so the modulo is a fast bit-mask (`hash & (cap-1)`).
- **0.75 load factor**: grow before the table is more than ¾ full. Emptier tables
  have shorter probe sequences (faster) but waste memory; 0.75 is the classic
  balance. Growing **re-inserts** every entry, because a key's home bucket
  depends on the capacity.
- **Tombstones for deletion.** You cannot just blank a deleted bucket — that
  would break the probe chain for keys after it. Instead a delete leaves a
  *tombstone* (`key == NULL`, `value == true`) that lookups skip over but inserts
  may reuse. Getting this right is the single trickiest part of an
  open-addressed table, and `table.c` is commented line-by-line on it.

### 11.3 String interning (`copyString` + `tableFindString`)

The same `Table`, used as a **set**, holds every live string. Before creating a
string, `copyString` checks that set; if an identical one exists, it returns the
*existing* object. The payoff is enormous: any two equal strings are then the
**same pointer**, so string equality (and hash-table key comparison) is a single
pointer compare instead of a byte-by-byte `memcmp`. This is why `findEntry` can
write `entry->key == key`, and why `"ab" == "a" + "b"` is `true` and O(1). Hashes
are computed once (FNV-1a) and cached on the object.

### 11.4 The three global opcodes and the define/assign distinction

Variable names are stored as string constants; each opcode carries a one-byte
index to its name and uses it as a key into `vm.globals`:

- `OP_DEFINE_GLOBAL` — pops the initialiser and binds the name (overwrite OK).
- `OP_GET_GLOBAL` — looks the name up; **undefined name is a runtime error**.
- `OP_SET_GLOBAL` — must update an *existing* binding. It exploits `tableSet`'s
  return value (true = key was newly added): if assignment accidentally created
  the key, it deletes it back out and errors. Crucially, `SET` does **not** pop —
  assignment is an expression whose value flows on.

That last point is the deep one: `let x = …;` is a *statement* (net stack effect
zero, value consumed), but `x = …` is an *expression* (value left on the stack),
which is why `print x = 5;` prints 5 and `a = b = 1;` chains.

### 11.5 The l-value problem (`assignment` in `parser.c`)

`a = 1` reads left-to-right, but the left side is a *target* (where to store), not
a value. The clean recursive-descent solution: parse the left side as an ordinary
expression; if a `=` follows, check that what we built is assignable (a bare
`NODE_VAR_GET`) and rewrite it into an assignment, salvaging the name. Anything
else (`1 + 2 = 3`) is reported as "invalid assignment target." Assignment recurses
on its right side, making it right-associative, and sits at the lowest precedence.

> **Bonus that fell out:** because strings are now real values, `+` became
> **overloaded** — the VM checks operand types at runtime and either adds ints or
> concatenates strings (`concatenate()` in `vm.c`). That runtime dispatch is the
> essence of operator overloading in a dynamic language.

---

## 12. Case study: local variables, lexical scope, and compile-time resolution

Roadmap step 4, completed. Where globals are stored by name in a runtime hash
table, **locals are resolved to numeric stack slots by the compiler**, so reading
a local is a single array access with no name and no lookup. This step is almost
entirely in `compiler.c` — the VM barely changed — which is itself the lesson: a
lot of a language's behaviour is decided at *compile* time.

### 12.1 Two kinds of variable, one decision point

The compiler's `resolveLocal` searches its compile-time list of in-scope locals
for a name. If found, the variable is a local and the compiler emits a
slot-indexed `OP_GET_LOCAL`/`OP_SET_LOCAL`; if not, it falls back to the by-name
`OP_GET_GLOBAL`/`OP_SET_GLOBAL` from step 3. That single branch in
`NODE_VAR_GET`/`NODE_ASSIGN` is the entire global-vs-local distinction. The payoff
is visible in `--dump`: inside a block, `print a + b` becomes `OP_GET_LOCAL 0`,
`OP_GET_LOCAL 1`, `OP_ADD` — indices, not names.

### 12.2 The compile-time model of the runtime stack

The key idea: the compiler keeps a `locals[]` array that **mirrors what the VM
stack will look like** at runtime. When a local is declared, its slot index is
simply its position in that array — which is exactly where its value will sit on
the VM stack, because (a) every statement is stack-neutral, so the stack is empty
at statement boundaries, and (b) a local's initialiser leaves its value on top.
So "storing" a local needs **no opcode at all**: the value's natural stack
position *is* the variable. This elegant correspondence is why stack-based VMs
make locals so cheap.

### 12.3 Scopes: begin, end, and the cost of leaving

`beginScope`/`endScope` just bump and drop a `scopeDepth` counter. The real work
is at end of scope: every local declared in the block must be removed from the
runtime stack, so `endScope` emits one `OP_POP` per local and shrinks the
compile-time model to match. That is the concrete runtime cost of a block —
proportional to the locals it declared. (Real VMs add an `OP_POPN` to pop many at
once; we keep it explicit.)

### 12.4 Three correctness subtleties real compilers handle

- **Shadowing.** `resolveLocal` scans from the innermost local outward, so an
  inner `let x` is found before an outer one — it *shadows* it. On block exit the
  inner slot is popped and the outer reappears. The example prints `1 2 3 2 1`.
- **Redeclaration in the same scope.** `declareLocal` rejects a second `let` of
  the same name *at the same depth* (a likely bug) while still allowing an inner
  scope to shadow. This is a compile-time error.
- **Self-reference in an initialiser.** `let e = e;` is nonsense. We declare the
  name at depth `-1` ("uninitialised") *before* compiling its initialiser, and
  `resolveLocal` errors if it sees a `-1` local — catching the self-reference.
  Only after the initialiser compiles do we `markInitialized`.

### 12.5 Compile-time errors, cleanly

Because the compiler can now fail (the three cases above, plus "too many
locals"), `compile()` returns a bool and records errors via `compileError`
instead of `exit()`-ing mid-stream — mirroring the parser's report-and-continue
style. `interpret()` checks the result and skips execution on failure.

### 12.6 A real bug this step exposed (and the fix)

Adding `}` to the language surfaced a latent **infinite loop** in error recovery
from step 2. A token that cannot start an expression (a stray `}`) was reported
by `primary()` but never *consumed*; the recovery loop could early-return from
`synchronize()` on a stale `;` and then retry the very same token forever,
flooding errors until the process was OOM-killed. The fix is a one-liner with a
big principle behind it: **an error path must still make forward progress.**
`primary()` now consumes the offending token on error, guaranteeing the parser
always advances. The test suite gained regression guards (`}`, `} } }`, `* 3;`)
that must *terminate*, not hang. Lesson: test your error paths, not just your
happy paths — and a parser that can loop on malformed input is a real
denial-of-service bug, not a cosmetic one.

---

## 13. Case study: control flow, jumps, and backpatching

Roadmap step 5, completed — the step that makes cnano **Turing-complete**. Until
now execution ran straight through, top to bottom. Control flow means *changing
the instruction pointer*, and the entire mechanism is three jump opcodes plus one
compile-time technique: backpatching.

### 13.1 Jumps with two-byte operands

`OP_JUMP` (always), `OP_JUMP_IF_FALSE` (conditional), and `OP_LOOP` (always,
backward) each carry a **2-byte** offset, so one jump can span up to 65535 bytes.
A one-byte offset (like our constant indices) would cap a loop body or `if` at
256 bytes — too small for real code. This is the same operand-size trade-off seen
with constants, resolved the other way because jumps must reach further.

### 13.2 Backpatching: the chicken-and-egg of forward jumps

When the compiler emits a forward jump (to skip an `if`'s then-branch, say), it
does **not yet know** how far to jump, because the code being skipped hasn't been
compiled. The fix is **backpatching**:

1. `emitJump` writes the opcode + a **placeholder** `0xffff`, and returns the
   placeholder's location.
2. The compiler compiles the code to be skipped.
3. `patchJump` goes back and overwrites the placeholder with the now-known
   distance from the jump to "here."

Backward jumps (`emitLoop`) need no patching: the target is already behind us, so
the distance is known immediately. That asymmetry — patch forward, compute
backward — is worth holding onto.

### 13.3 `if` / `while` and the symmetric POP discipline

The cleverest detail is that **`OP_JUMP_IF_FALSE` does not pop** its condition.
That single choice lets the same opcode serve `if`, `while`, *and* short-circuit
`and`/`or`. The price is that each control-flow construct must explicitly pop the
condition on **both** paths (taken and not-taken) to keep the stack balanced. You
can see the symmetric pair of `OP_POP`s in both the `if` and `while` emitters. If
either pop is missing, the stack drifts by one per iteration — which a long loop
turns into an overflow. (Our `loop-balance` test runs 500 iterations precisely to
catch that class of bug; the stack cap is 256.)

A `while` is just `if`'s forward exit-jump plus an `OP_LOOP` back to re-test the
condition. Run `--dump` on a loop to see `JUMP_IF_FALSE … -> end` and
`LOOP … -> condition`.

### 13.4 `for` is pure sugar; `and`/`or` are jumps, not operators

Two reuse lessons land here:

- **`for` has no node and no opcode.** The parser desugars
  `for (init; cond; update) body` into `{ init; while (cond) { body; update; } }`
  — built entirely from the block and while nodes we already had. Desugaring a
  whole *statement form* (not just an operator like `!=`) shows how far the
  "small core, convenient surface" idea scales. `else if` likewise needs no
  special syntax: it is just an `else` whose statement happens to be another
  `if`.
- **`and`/`or` short-circuit, so they cannot be plain binary operators** (those
  evaluate both sides). They are compiled as jumps: evaluate the left, then
  conditionally jump over the right. Because `JUMP_IF_FALSE` leaves the condition
  on the stack, the deciding operand naturally *becomes the result* —
  `nil or "x"` yields `"x"`, and the skipped side genuinely never executes (our
  tests prove no side effect occurs).

---

## 14. Case study: functions, call frames, and the calling convention

Roadmap step 6, completed — the conceptual heart of the language, and the step
that ties together everything from steps 1–5. It touches every file, but the new
ideas cluster around two things: **functions as objects that own their own
bytecode**, and a **call-frame stack** at runtime.

### 14.1 Functions are objects with their own chunk

`ObjFunction` (in `object.{h,c}`) is a heap object — same `Obj` header, same
intrusive free list as strings — but it carries a whole `Chunk`. Each function is
compiled independently into its own bytecode. The masterstroke is uniformity:
**the top-level script is itself a function** (an implicit `<script>`), so the VM
only ever runs functions and the bootstrap is just "push the script function and
call it with zero arguments." One code path handles everything.

### 14.2 One compiler per function

The compiler grew a `CompilerState` *per function*, linked by an `enclosing`
pointer into a stack that mirrors the nesting of `fn` declarations.
`compileFunction` spins up a fresh state (its own chunk, its own locals starting
at slot 0) and pops it when done. Parameters are simply the function's first
locals — at runtime the caller will have placed the arguments in exactly those
slots, so no copying is needed. Slot 0 is reserved for the callee itself, which
is why it lines up with the runtime frame (below).

### 14.3 The call-frame stack — the runtime heart

A `CallFrame` is the activation record of one in-progress call. It holds the
function, that call's **own `ip`**, and `slots` — a pointer into the shared
operand stack marking where this call's window begins. The key identity:

> a local at compile-time **slot i** is, at runtime, simply **`frame->slots[i]`**.

Because `slots` is different on every (re)entry, the *same bytecode* reads and
writes a *different physical location* each call — which is exactly what gives
recursion its independent locals. fib(20) runs ~13,000 overlapping calls, each
with its own `n`, on one flat stack.

### 14.4 The calling convention, both sides

The contract that lets caller and callee cooperate:

- **Compiler (caller):** push the callee, then the arguments left-to-right; emit
  `OP_CALL argc`. The stack is now `[.. callee arg0 .. argN]`.
- **VM (`OP_CALL`):** the callee sits `argc` below the top. `call()` checks arity,
  pushes a new `CallFrame` whose `slots` points at the callee, and execution
  continues in the callee's chunk (we re-cache the `frame` local).
- **VM (`OP_RETURN`):** grab the result, reset `stackTop` back to `frame->slots`
  (tearing down the callee's entire window — locals, args, callee — in one move),
  push the result there, and resume the caller. The whole frame vanishes at once.

This window-based teardown is why functions are cheap and why a returning
function automatically frees all its locals.

### 14.5 What falls out for free, and the safety rails

- **Recursion and mutual recursion** need no special support: globals resolve by
  name at call time, so a function can call itself or peers declared anywhere.
- **First-class functions:** a function is just a `Value`, so `let f = sq;` works.
- **Stack traces:** because every frame keeps its own `ip`, `runtimeError` walks
  the frames innermost-to-outermost and prints `in c()` / `in b()` / `in script`.
- **Safety:** arity mismatch, calling a non-function, infinite recursion
  (a controlled "stack overflow" at `FRAMES_MAX`), and `return` at top level are
  all clean errors — never a host crash.

> **Note on closures.** Step 6 deliberately stopped before capturing enclosing
> *locals*. §15 builds exactly that on top of the per-function compiler set up
> here.

---

## 15. Case study: closures and upvalues

The natural completion of functions — and one of the most elegant mechanisms in
language implementation. A nested function can use a variable from an enclosing
function; the difficulty is that the variable lives on the stack and may
**outlive** the frame that created it (return a counter, and its `count` must
survive `makeCounter` returning). The solution is **upvalues** that migrate a
captured variable from the stack to the heap at exactly the right moment.

### 15.1 The problem, concretely

```
fn makeCounter() {
  let count = 0;             // a LOCAL — lives in makeCounter's stack frame
  fn increment() { count = count + 1; return count; }
  return increment;          // makeCounter's frame is torn down here ...
}
let c = makeCounter();
c(); c();                    // ... but `count` must still exist. Where?
```

Once `makeCounter` returns, its stack slot for `count` is gone. Yet `increment`
must keep reading and writing the *same* `count`. So the variable has to leave
the stack and move somewhere durable — the heap.

### 15.2 Two new objects: closures and upvalues

- An **`ObjClosure`** is a function plus the array of variables it captured. The
  VM now calls closures, never bare functions (a plain function is just a closure
  with zero upvalues), which keeps the call path uniform.
- An **`ObjUpvalue`** is the captured variable's "box," with one level of
  indirection (`location`). While the variable is still on the stack the upvalue
  is **open**: `location` points at the stack slot, so the closure and the
  original code share it live. When the slot is about to vanish, the upvalue is
  **closed**: the value is copied into the upvalue's own `closed` field and
  `location` is repointed there. Same pointer dereference works before and
  after — the indirection hides the move.

### 15.3 The compiler: resolving captures (`resolveUpvalue`)

Variable resolution becomes three-way: **local → upvalue → global**.
`resolveUpvalue` is recursive and is the clever core:

1. Is the name a local of the *immediately enclosing* function? Mark that local
   as **captured** and add an upvalue pointing at its slot (`isLocal = true`).
2. Otherwise, ask the enclosing function to resolve it as one of *its* upvalues,
   and chain to that (`isLocal = false`).

That recursion is what lets a closure reach a variable two or more levels up:
each intervening function captures it and passes it along, hop by hop. Upvalues
are **de-duplicated** per function so a shared variable maps to one upvalue —
essential so sibling closures see each other's writes.

### 15.4 The runtime: capturing and closing

- **`OP_CLOSURE`** (our only variable-length instruction) builds the closure and,
  for each upvalue, either captures a current-frame slot (`captureUpvalue`) or
  copies an upvalue from the enclosing closure — exactly mirroring the compiler's
  local/upvalue decision.
- **`captureUpvalue`** keeps a list of open upvalues sorted by slot and reuses an
  existing one for a slot, so all closures over the same variable share one box.
- **`closeUpvalues`** runs when captured locals leave scope (`OP_CLOSE_UPVALUE`)
  and on every `OP_RETURN`: it copies the affected values off the stack into
  their boxes and repoints them. This is the precise moment a variable's lifetime
  is extended beyond its frame.

### 15.5 Why the design is worth studying

The open/closed upvalue with shared boxes makes all the tricky cases fall out:
independent counters (separate captures), shared mutable state between sibling
closures (one shared box), captured parameters, and multi-level capture (chained
upvalues) — all verified in the tests and `examples/closures.cn`. It is also the
standard technique used by real VMs (Lua, and clox in *Crafting Interpreters*).

> With closures, cnano has the full core of a small dynamic language: data types,
> variables and scope, control flow, and first-class functions that close over
> their environment.

---

## 16. Case study: a gradual static type checker

Roadmap step 7, completed — a different *kind* of step. Instead of adding a
runtime feature, we add a **separate analysis pass** that runs between parsing and
compilation, computes a type for every expression, and rejects provably-wrong
programs **before they run**. This is your first taste of *static analysis*:
reasoning about behaviour from structure alone. The new pipeline stage:

```
... → Parser → AST → [Type checker] → Compiler → ...
```

### 16.1 Static vs. dynamic — and the gradual middle

A dynamically typed language (what cnano was) checks types at *runtime*: flexible,
no annotations, but errors surface only when the bad line executes. A statically
typed language checks *before* running: errors caught early and uniformly, at the
cost of annotations and some rigidity. cnano takes the modern middle path —
**gradual typing** (à la TypeScript, Python's hints, Dart):

- Annotations are **optional**: `let x: int = 5;`, `fn add(a: int): int {...}`.
- Anything unannotated has the type **`any`**.
- **`any` is compatible with everything**, in both directions (`compatible()` in
  `typecheck.c`). That one rule is the whole escape hatch: typed and untyped code
  interoperate, and an error fires *only* when both sides are known and genuinely
  disagree. This is why all 138 pre-existing untyped tests still pass untouched.

Types are then **erased**: the checker is purely a gate. The compiler and VM are
unchanged and remain dynamically typed. (A production language would also use the
types to *optimise* — unboxing, devirtualisation — but erasure keeps the lesson
focused.)

### 16.2 The checker is a third tree walk

By now the AST-walk shape is familiar: the lexer/parser build the tree, the
compiler walks it to emit code, and now the checker walks it to compute types.
`checkExpr` returns a `Type *` for each expression; `checkStatement` validates
statements. It carries a lexically-scoped symbol table (name → type), mirroring
the compiler's locals array, with `beginScope`/`endScope`. The operator rules are
the heart: `+` is type-aware (int add *or* str concat, matching the VM's runtime
overload), arithmetic/comparison `requireInt`, `==` accepts anything, `!` yields
bool. Each rule reports an error only on a *known* mismatch — never on `any`.

### 16.3 Two passes, so functions can be forward-referenced

Functions are checked in **two passes** over the top level:

1. **Register** every top-level function's *type* (from its annotations) into the
   symbol table.
2. **Check** every statement, including function bodies.

Pass 1 is what lets a call resolve a function declared *later*, and lets
**mutually recursive** functions check each other — the static analogue of the
hoisting the runtime already had for globals. A function's body is checked with
its parameters bound to their annotated types and its declared return type
recorded, so every `return` is validated against it.

### 16.4 A little inference, and what it catches

When a `let` has no annotation, its variable takes the *inferred* type of its
initialiser (`let y = x + 1;` makes `y` an int if `x` is) — a first taste of type
inference. Statically caught errors include: bad initialiser type, `int + bool`,
wrong argument type, wrong **arity** (before running!), wrong return type, calling
a non-function, negating a bool, and comparing a str to an int — each reported
with a line number and the offending types, then compilation is refused.

### 16.5 Memory note

Function types are heap-allocated (they carry parameter/return arrays) and tracked
in a small **arena** freed all at once when checking finishes — simpler and
leak-free versus per-type lifetime tracking. Primitive types are singletons.

> Gradual typing is a genuine, current research-to-practice design. cnano's
> version is small but real: it demonstrates the core compatibility rule, scoped
> symbol resolution, two-pass forward references, and type-directed error
> reporting — the skeleton every real checker shares.

---

## 17. Case study: optimisation — constant folding

Roadmap step 8, completed. The first optimisation pass: do at *compile* time work
the VM would otherwise repeat at *run* time. It slots in as another AST pass,
after type-checking and before compilation:

```
... → Type checker → [Optimiser] → Compiler → ...
```

### 17.1 Constant folding as a bottom-up rewrite

`foldConstants` (in `optimize.{h,c}`) walks the tree and, wherever a subexpression
is made entirely of literals, replaces it with the computed literal. `2 + 3 * 4`
becomes `14`; `!!false` becomes `false`; `"a" + "b"` becomes `"ab"`. The pattern
is **bottom-up**: fold the children first, then check whether *this* node's
children are now literals and can collapse. Because we mutate the tree in place
(freeing the replaced subtrees), the unchanged compiler then emits much smaller
bytecode — a single `OP_CONSTANT 14` instead of three constants and two adds.

### 17.2 Two correctness traps the folder must respect

Optimisation is only valid if it preserves behaviour. Two cases here are easy to
get wrong:

- **Division by zero must NOT be folded.** `1 / 0` has to stay a *runtime* error,
  so the folder explicitly declines to fold a division with a zero divisor and
  leaves the node for the VM. An optimiser that "simplified" it would change the
  program's observable behaviour.
- **`and`/`or` are never folded.** They short-circuit, so folding could elide a
  side effect on the skipped branch. The folder recurses into their operands but
  leaves the logical node intact. (Verified: `false and (x = …)` still doesn't
  run the assignment.)

These mirror cnano's own semantics exactly — including the truthiness rule, so a
folded `!0` is `false` just as the VM would compute.

### 17.3 Constant deduplication and `OP_CONSTANT_LONG`

Two smaller wins, both about the constant pool:

- **Dedup** (`makeConstant`): before adding a constant, reuse an equal existing
  one. A variable name or repeated literal now occupies a single pool slot no
  matter how often it appears — smaller chunks, and far fewer slots used.
- **`OP_CONSTANT_LONG`**: the original `OP_CONSTANT` had a one-byte index, capping
  a chunk at 256 constants (flagged way back in step 0). `emitConstant` now emits
  the compact 1-byte form when the index fits and a 3-byte form otherwise — small
  code in the common case, no hard cap in the rare one.

> A real bug this step surfaced: the by-name global opcodes still carry a
> one-byte name index, so 256+ *distinct* global names would have overflowed it
> and read the wrong name. With dedup that is now hit only by genuinely 256+
> distinct names, and `identifierConstant` reports a clean compile error instead
> of silently corrupting — the right call when a real fix (long global opcodes)
> would add bulk for a case almost no program reaches. Honest limits, clearly
> reported, beat silent wrong answers.

---

## 18. Case study: native code generation (compiling to C)

Roadmap step 9, completed — the leap from *interpreted* to *compiled, ahead of
time*. Instead of feeding the AST to the bytecode compiler, a second backend
walks it and emits **C source**, which the system `cc` turns into a standalone
native executable. The program then runs with **no cnano runtime at all**.

```
                                  ┌→ Compiler → bytecode → VM        (default)
... → Type checker → Optimiser →  ┤
                                  └→ C backend → cc → native binary  (--native)
```

### 18.1 Why C is a legitimate target

Emitting C ("transpiling") is a real, widely-used technique — Nim, Vala, and
early C++ all did it. C acts as a **portable assembler**: we inherit the system
compiler's optimiser and every CPU it targets, for a fraction of the effort of
emitting machine code or LLVM IR by hand. The trade-off is a dependency on a C
compiler at build time and slightly less control than raw codegen — a good
trade for a teaching project, and a real one in production.

### 18.2 Why types make this pay off — and bound the scope

The backend compiles only the **statically-typed, first-order subset**. That
restriction is the whole point: because the type checker has proven each value's
type, the generated C is **unboxed** — a cnano `int` is a C `int64_t`, a `bool` a
C `bool`, a `str` a `const char *`. No tagged unions, no dynamic dispatch, no
boxing: genuinely fast native code, not an interpreter in disguise. This is a
concrete lesson in *why static types enable optimisation*.

Features needing a runtime — closures (heap upvalues), dynamic `any`, first-class
functions — are **rejected** by the backend with a clear message pointing back to
the VM. Splitting a language into a fast specialised path plus a general dynamic
path is exactly how real systems layer (think JITs, or `nogc`/`unsafe` subsets).

### 18.3 The mapping is mostly structural

cnano and C share an expression/statement structure, so most of the backend is a
direct transcription: `if`/`while` map to `if`/`while`, operators to operators,
a typed `let` to a C declaration, a function to a C function. A few details carry
real lessons:

- **Name mangling.** Every cnano identifier is emitted with a `cn_` prefix, so a
  cnano variable literally named `int` (legal — type names aren't reserved) can
  never collide with a C keyword or our helpers.
- **Forward declarations.** Pass 1 emits a C prototype for every function before
  any body, so calls resolve regardless of order — the same forward-reference and
  mutual-recursion support the type checker and VM already had, expressed in C's
  own mechanism. Globals are likewise file-scoped first, then initialised in
  order inside `main`.
- **A tiny runtime prelude.** Two helpers only: `cn_concat` (string `+`) and
  `cn_div` (turns divide-by-zero into the same controlled error+exit cnano
  guarantees). Typed code needs almost nothing else — the contrast with the
  bytecode VM's machinery is the lesson.
- **Type-directed printing.** `print` picks its `printf` format from the
  expression's inferred type (`%lld`, `true`/`false`, `%s`).

### 18.4 What this demonstrates

`cnano --native fib.cn -o fib` produces a real ELF/Mach-O executable that runs
with zero interpreter overhead and — verified by the test suite — produces output
**identical** to the VM. Seeing the same `factorial`/`fib`/FizzBuzz program run
both ways, and reading the readable C in between (`--emit-c`), makes the whole
"what is compilation" question concrete: it is just a structured translation from
one representation to a lower-level one, repeated until you reach the machine.

---

## 19. Case study: automatic memory management (a garbage collector)

Until this step, cnano allocated heap objects and freed them all *at once* at
shutdown (`freeObjects`). That is fine for a program that runs briefly, but a
loop that builds endless temporaries would grow without bound — nothing reclaims
the dead ones *while the program runs*. Step 10 fixes that with a real tracing
**mark-and-sweep** garbage collector (`memory.{h,c}`).

### 19.1 The idea: reachability = liveness

A value is still "live" only if the running program can still *reach* it by
following references from a **root** (a value the program holds directly: the
operand stack, call frames, globals, open upvalues). Anything not reachable can
never be used again, so its memory is safe to reclaim. The collector makes this
concrete in two phases:

- **Mark** — start at the roots and trace every reference, marking each reachable
  object. We use the **tri-colour** abstraction (white = untouched, grey = found
  but its own references not yet scanned, black = fully scanned) with an explicit
  grey **worklist**, so tracing is an iterative loop, not deep recursion that
  could itself overflow the stack.
- **Sweep** — walk the intrusive list of *all* objects and free every one still
  white. Survivors are flipped back to white for the next cycle.

### 19.2 The roots are the whole game

`markRoots()` is the single most important function: miss a root and the GC frees
something still in use. cnano's roots are exactly its execution state — stack
slots, each frame's closure, the open-upvalue list, and the globals table. The
test of correctness is `make gcstress`: collect on **every** allocation, under
AddressSanitizer, running the full suite. If any root were missed, a live object
would be freed and ASan would report a use-after-free. Identical output to the
normal build is the proof that liveness is computed correctly.

### 19.3 Two subtleties worth the chapter

- **The weak intern table.** The string-intern pool (`vm.strings`) points at every
  live string — but it must *not*, by itself, keep a string alive, or no string
  would ever be collected. It is a **weak** table: excluded from the roots, and
  pruned of now-dead entries (`tableRemoveWhite`) after tracing but *before*
  sweeping, so it never ends up pointing at freed memory. Weak references are a
  recurring real-world GC concept (caches, interning, listeners) and this is the
  smallest honest example of one.
- **Why the collector is quiescent during compilation.** A GC may only run when it
  can see *all* live objects through roots. cnano builds a separate **AST** that
  holds `ObjString*` pointers which are *not* reachable from any runtime root, so
  collecting mid-compile would free strings the AST still needs. Rather than paper
  over this (e.g. by rooting the AST), we make a clean design choice: the
  collector stays off until execution begins (`vm.gcEnabled`), by which point
  compilation is done and the AST is freed. A single-pass compiler (like *Crafting
  Interpreters*') avoids the issue differently — by funnelling constants straight
  into the chunk as it goes. Naming *why* our architecture differs is the lesson.

### 19.4 Self-tuning, and what it demonstrates

`reallocate()` is the one allocation choke point; it keeps a running byte tally
and triggers a collection once the heap grows past a threshold that is re-set to a
multiple of the live set after each cycle — so GC frequency scales with how much
the program actually retains. Watching a 20 000-iteration closure-churn loop hold
**flat** at a few hundred live bytes (instead of climbing into the megabytes)
makes the payoff tangible: automatic memory management is not magic, it is
reachability analysis run periodically over a well-defined root set.

---

## 20. Case study: a structured type system

cnano's checker started with a flat `TypeKind` **enum** — `int`, `bool`, `str`,
`nil`, `any`, and a special `fn`. That is enough while every type is atomic, but
the moment you want `[int]` (a list of ints) or `{str: int}` (a map from strings
to ints), an enum collapses: `[int]` and `[bool]` are both "array", yet they are
*different types*. Step 11 generalises the representation so the checker can tell
them apart — the foundation the upcoming array and map values will stand on.

### 20.1 From enum to a tagged, recursive type

A type is now a small struct (`type.h`): a `kind` tag plus, for the PARAMETRIC
kinds, the types it is built from — an array carries its `element` type, a map its
`key` and `value`, a function its parameters and return. Because a type can
*contain* types, the structure is **recursive**, and so is everything that walks
it. `[[int]]` and `{str: [int]}` fall out for free.

Primitive types stay **shared singletons** (one `int` type, compared by pointer);
only the parametric types are heap-allocated, in a small **arena** that is freed
in one call (`freeTypes()`) once every pass that reads types — the checker *and*
the native codegen — is done. That "allocate freely, free all at once" arena is a
recurring pattern for compiler data with a clear, common lifetime.

### 20.2 Compatibility recurses

The gradual rule is unchanged at the top — `any` matches anything — but for equal
parametric kinds, compatibility now recurses into the parts: two arrays are
compatible iff their elements are; two maps iff both key and value are. So `[int]`
is compatible with `[int]` and with `[any]`, but not with `[bool]`, and the error
points at exactly the mismatch. This is **structural** typing in miniature, and
the same three lines handle nesting to any depth.

### 20.3 Two details that bite in practice

- **The annotations moved onto the AST as full `Type*`.** Previously the parser
  stored a bare `TypeKind`; now it builds a real (possibly nested) `Type` while
  parsing `[T]` / `{K: V}`, so the structure is captured at parse time and simply
  read later. Type names remain non-reserved — `int` is recognised only in
  annotation position, so it is still usable as a variable name.
- **Naming composite types needs more than one buffer.** `typeName` must format
  strings like `"[int]"`, but error messages routinely use *two* names at once
  (`expects %s but got %s`). A single static buffer would let the second call
  clobber the first; a small **ring** of buffers keeps several recent names valid
  simultaneously. A tiny detail, but the kind that produces baffling error
  messages if you get it wrong.

### 20.4 What it demonstrates

There are no array or map *values* yet — this step is pure type machinery — but it
is already exercised end-to-end: you can write `[int]`/`{str: int}` annotations on
parameters, returns and `let`s, and the checker accepts the matching ones and
rejects structural mismatches with precise messages. The native backend, whose
scalar subset has no place for heap collections, cleanly rejects them. The lesson
is that *representation drives capability*: enriching the type from an enum to a
recursive structure is what makes every later collection feature expressible.

---

## 21. Case study: built-in functions and method dispatch

A language needs to reach outside itself — to read the clock, format a value, ask
a string its length. cnano had no way to do any of that: every callable was a
user-defined bytecode function. Step 12 adds two complementary mechanisms, both
implemented in C, and both prerequisites for giving arrays and maps an API.

### 21.1 Native functions: callables implemented in C

A new object type, `ObjNative`, wraps a C function pointer. We register a few as
globals at startup (`clock()`, `str(x)`), so they are looked up by name like any
variable and called with the ordinary `OP_CALL`. The VM's `callValue` grows one
arm: if the callee is native, it checks arity, calls the C function with the
arguments straight off the stack, and replaces callee+args with the result — *no
call frame is pushed*, because there is no bytecode to run. That asymmetry (frames
for cnano functions, none for natives) is exactly how CPython, Lua and clox treat
their C builtins.

The signature is worth noting: a native returns `bool` and writes its result
through a pointer, rather than just returning a `Value`. That extra channel lets a
builtin *fail cleanly* — report a runtime error and return false — which `str()`
doesn't need but array indexing (bounds checks) soon will. Designing the boring
case for the hard one that's coming is usually cheaper than retrofitting it.

### 21.2 Method calls: dispatch on the receiver's type

`receiver.method(args)` compiles to a single fused instruction, `OP_INVOKE`,
carrying the method-name constant and the argument count — the same shape clox
uses, and faster than a separate "get property" then "call". cnano has no
user-defined types, so there is nothing to look the method up *on* except the
receiver's built-in TYPE: the VM hands the receiver and arguments to
`invokeMethod`, which picks a per-type table (strings have one; arrays and maps
will get theirs) and finds the method by name. A miss, a wrong arity, or a
receiver type with no methods each become a clean runtime error.

Splitting the work is the lesson here: the VM stays tiny (read the name, find the
receiver, delegate), while all the actual behaviour lives in `builtins.c`. Adding
`"abc".upper()` later means adding one row to a table — no VM change at all.

### 21.3 Where each layer draws the line

- **The type checker** treats a method call as `any`: builtin signatures live in
  C, outside the gradual type system, so it walks the receiver and arguments for
  errors but defers the method itself to runtime. Pragmatic gradual typing.
- **The native backend** rejects both method calls and the heap builtins: they
  need the dynamic runtime its unboxed scalar subset deliberately lacks — the same
  boundary every non-scalar feature respects.

The payoff: `"hello".len()` returns `5`, `str(7)` lets you build `"n=" + str(7)`,
and the dispatch machinery is now in place — so the array and map steps are mostly
"fill in the method tables".

---

## 22. Case study: arrays — the first aggregate type

With GC, structured types, and method dispatch all in place, the first real
collection is mostly *assembly*. An array is a new heap object, `ObjArray`, that
wraps the very same `ValueArray` (count/capacity/double-on-full) the constant pool
has used since step 0 — so the only genuinely new code is a literal, indexing, and
three methods.

### 22.1 Three small opcodes

- `OP_BUILD_ARRAY count` turns the top `count` stack values into a fresh array.
- `OP_INDEX_GET` pops an index and an object and pushes `object[index]`.
- `OP_INDEX_SET` takes `[.. object index value]`, stores, and leaves `value` (so
  `a[i] = x` is an expression yielding `x`, like every other assignment).

Indexing is **bounds-checked** in the VM: an out-of-range or non-int index is a
clean runtime error, never a memory stomp. That guarantee — the interpreter
refuses to read outside the array — is exactly what a language buys you over raw
C, and it costs two comparisons.

### 22.2 The l-value twist in the parser

`a[i]` is parsed as a postfix `OP_INDEX_GET`, the same as a read. Only when a `=`
follows does `assignment()` *rewrite* that get node into an `OP_INDEX_SET`,
salvaging the object and index subtrees. This is the identical trick the parser
already used to turn a variable read into a variable assignment — generalised to
indexed targets. One mechanism, two l-value shapes.

### 22.3 GC has to trace elements — and building is the tricky moment

An array keeps its elements alive, so the collector's `blackenObject` marks every
element. The subtle moment is *constructing* an array: the object is allocated
while its elements are still on the stack (so a GC triggered by that very
allocation can see them), and only then copied in — the elements are never
orphaned in the window between "allocated" and "rooted". The `gc-array-live` test
(2000 throwaway arrays churned while one live array grows to 2000 elements, run
under `make gcstress`) is what proves the tracing is right.

### 22.4 Types, methods, and the dynamic escape

The `[T]` types from step 20 finally carry values: a literal's element type is
*inferred* (`[1,2,3]` is `[int]`), indexing a `[T]` yields a `T`, and storing the
wrong type is caught before the program runs — all by reusing the structural
`compatible()`. The three methods (`.len()`, `.push()`, `.pop()`) are just rows in
a table `invokeMethod` already knew how to search. And because the element type is
*inferred*, `let a = [1,2,3]` makes `a` an `[int]` (just as `let x = 5` makes `x`
an `int`); `[any]` is the deliberate opt-out back to heterogeneous, fully dynamic
arrays. Capability that took a whole language's worth of machinery to enable, added
in one focused step.

---

## 23. Case study: maps — value-keyed hash tables

Maps complete the data-structure arc, and they force the one piece cnano had so
far avoided: **hashing arbitrary values**. The existing `Table` (globals, string
interning) keys only on interned `ObjString*` and compares by pointer. A map must
accept `1`, `true`, `nil`, and `"key"` as keys — so it needs its own table built
on *value* hashing and *value* equality.

### 23.1 A second hash table, by necessity

`ObjMap` is open-addressed with linear probing, like the string `Table`, but two
things differ. First, the key is a full `Value`, hashed by a new `hashValue`
(strings reuse their cached hash; integers get a bit-mix so neighbours scatter)
and compared with the existing `valuesEqual`. Second, there are **no tombstones**:
maps don't support deletion yet, and — crucially — there is no spare `Value` to
use as an "empty" sentinel the way `NULL` marks an empty string bucket (`nil` is a
*legal key*). So each slot carries an explicit `occupied` flag instead. Two
near-identical tables, kept separate precisely because their key model differs —
sometimes the honest design is duplication, not a forced abstraction.

### 23.2 What's hashable — and the reuse payoff

Only primitives and strings are hashable keys; an array or another map as a key is
a clean runtime error (`isHashableKey` gates every access). Everything else was
*free*: `m[k]` reuses the exact `OP_INDEX_GET`/`OP_INDEX_SET` opcodes arrays
introduced — the VM just dispatches on the object's type — and `.len()/.has()/
.keys()` are three more rows in the method table `invokeMethod` already walks.
Step 13 genuinely paid for step 14.

### 23.3 Types, GC, and two deliberate choices

`{K: V}` types finally carry values: a literal's key and value types are inferred
independently, indexing yields the value type, and structural `compatible()`
catches a wrong key or value type before the program runs — recursing into
`{str: [int]}` for free. The collector marks **both** key and value of every live
entry (the `gc-map-live` torture test churns 2000 maps under stress GC while one
grows to 2000 entries). Two design decisions worth naming: reading a **missing key
is an error**, not a silent `nil` (a `nil` would violate the value type `V` of a
typed map — type soundness beats convenience, and `.has()` is the guard); and
map **iteration is bucket order**, an honest consequence of a hash table that we
document rather than paper over.

The result: `{"alice": 30}["alice"]`, int/bool/nil keys, typed `{str: int}`
records — a real dictionary, built mostly from machinery that already existed.

---

## 24. Roadmap: where to go next

Each step below is a self-contained project that teaches a new concept. They are
ordered so each builds on the last.

1. ~~**Booleans, `nil`, and comparisons.**~~ **✅ DONE** — see §9 above for a
   full write-up. `Value` is now a tagged union; `<  <=  >  >=  ==  !=  !` work;
   the VM type-checks operands at runtime.
2. ~~**Statements and `print`.**~~ **✅ DONE** — see §10 above. Programs are now
   statement sequences; `print`/`OP_PRINT`, expression statements/`OP_POP`,
   panic-mode error recovery, and `//` comments all landed.
3. ~~**Global variables.**~~ **✅ DONE** — see §11 above. Built a from-scratch
   hash table (open addressing, tombstones), heap objects with interning, string
   values, and `let`/read/assign with l-value handling.
4. ~~**Local variables and scope.**~~ **✅ DONE** — see §12 above. `{ }` blocks,
   locals resolved to stack slots at compile time, shadowing, and compile-time
   checks for redeclaration and self-referential initialisers.
5. ~~**Control flow.**~~ **✅ DONE** — see §13 above. `if`/`else`, `while`, and
   `for` (desugared to block + while) via `OP_JUMP`/`OP_JUMP_IF_FALSE`/`OP_LOOP`
   and backpatching, plus short-circuiting `and`/`or`. cnano is now
   Turing-complete.
6. ~~**Functions and a call stack.**~~ **✅ DONE** — see §14 above. `fn`,
   parameters, `return`, a per-call frame stack and calling convention,
   recursion / mutual recursion, first-class functions, and stack traces.
6b. ~~**Closures.**~~ **✅ DONE** — see §15 above. Upvalues (open→closed),
   three-way local/upvalue/global resolution, chained and shared captures.
7. ~~**A type checker.**~~ **✅ DONE** — see §16 above. A gradual static checker:
   optional `int`/`bool`/`str`/`nil`/`any` annotations, an `any`-compatible-with-
   everything rule, two-pass function checking, light inference, and type errors
   (init/operator/arg/arity/return/callability) reported before execution.
8. ~~**Optimisations.**~~ **✅ DONE** — see §17 above. AST constant folding (with
   correct handling of div-by-zero and short-circuit), constant dedup, and
   `OP_CONSTANT_LONG` to lift the 256-constant cap.
9. ~~**Toward native code.**~~ **✅ DONE** — see §18 above. A second backend
   compiles the typed first-order subset to C, then to a standalone native
   executable via the system `cc` (`--native` / `--emit-c`); unboxed values,
   forward declarations, name mangling, output identical to the VM.

10. ~~**Garbage collector.**~~ **✅ DONE** — see §19 above. A mark-and-sweep
    tracing collector with a tri-colour grey worklist, a self-tuning heap-growth
    threshold, and a weak string-intern table; reclaims dead objects while the
    program runs (`make gcstress` collects on every allocation under ASan).

### The memory + data-structures arc (in progress)

With the collector in place, cnano is growing real aggregate data on top of it:

11. ~~**Structured type system.**~~ **✅ DONE** — see §20 above. The flat
    `TypeKind` enum became a tagged, recursive `Type` (arena-owned) so `[int]` and
    `{str: int}` are expressible and checked structurally; annotations now live on
    the AST as full `Type*`.
12. ~~**Method-call dispatch + builtins.**~~ **✅ DONE** — see §21 above.
    `OBJ_NATIVE` builtins (`clock`, `str`) as globals, plus postfix
    `a.method(args)` compiled to a fused `OP_INVOKE` that dispatches on the
    receiver's type via per-type method tables (strings: `.len()`).
13. ~~**Arrays** (`[T]`).~~ **✅ DONE** — see §22 above. `ObjArray` over the
    existing ValueArray; `OP_BUILD_ARRAY`/`OP_INDEX_GET`/`OP_INDEX_SET`
    (bounds-checked); `.len()/.push()/.pop()`; inferred element types, structural
    `[T]` checking, GC-traced elements.
14. ~~**Maps** (`{K: V}`).~~ **✅ DONE** — see §23 above. `ObjMap` is its own
    value-keyed hash table (general `hashValue` + `valuesEqual`, occupancy flags,
    no tombstones); literals + `OP_BUILD_MAP`; `m[k]` reuses the array index
    opcodes; `.len()/.has()/.keys()`; structural `{K: V}` checking; GC traces keys
    and values.

**The memory + data-structures arc (steps 10–14) is complete.** cnano now has a
garbage collector, a structured type system, builtin functions and methods, and
both array and map collections — typed or dynamic, all GC-managed.

### The ergonomics, objects & errors arc (steps 15–29, in progress)

With a complete core language, the next arc makes cnano practical to *use*, lets
programs define their own types, and makes failure recoverable.

**Phase A — ergonomics & practicality**
15. ~~**Modulo `%`.**~~ **✅ DONE** — new token/opcode/`OP_NODE_MOD`, int-typed,
    zero-divisor guarded (VM + native `cn_mod`), folded when constant. The
    smallest possible "a feature across every layer" exercise.
16. ~~**Compound assignment** (`+=`, `-=`, `*=`, `/=`, `%=`).~~ **✅ DONE** — pure
    parser desugaring (`x OP= e` → `x = x OP e`) for variable AND index targets,
    using a `cloneExpr` of the pure target so `a[i] += 1` neither double-frees nor
    double-evaluates; no new opcodes.
17. ~~**`for-in` iteration**.~~ **✅ DONE** — `for (let x in coll)` desugars (in
    the parser) to a block + index `while` over a hidden `$for_iter(coll)` that
    yields the array to walk (arrays → elements, maps → keys), so one shape covers
    both even when the type is dynamic. Disambiguated from C-style `for` with one
    token of lookahead after `let NAME`; added the `in` keyword.
18. ~~**Standard-library builtins & string/array methods.**~~ **✅ DONE** — free
    `len/type/assert/abs/min/max`; string `.upper/.lower/.contains/.indexOf/
    .substring`; array `.contains/.indexOf/.join/.sort` (in-place, homogeneous);
    map `.values`. All additive in builtins.c — no VM/checker changes.
19. ~~**Higher-order collection methods** (`.map`/`.filter`/`.reduce`).~~ **✅
    DONE** — arrays gain `.map/.filter/.reduce` that call a cnano function back
    from C. Made the VM **re-entrant**: `run()` takes a `stopFrame` and a new
    `callFromVM` pushes the callee+args, runs until that frame returns, and yields
    the result. The accumulator is rooted on the VM stack across callbacks, so it
    survives a GC triggered inside one (verified under `gcstress`). **Phase A
    (ergonomics) complete.**

**Phase B — user-defined types**
20. **Structs / records** — named fields, `.` get/set, `ObjInstance`, named types.
21. **Methods on user types** — `OP_INVOKE` extended to instances; `self`.
22. **Constructors / initializers.**
23. *(optional)* **single inheritance + `super`.**

**Phase C — errors & optional values**
24. **Nullable / optional types** (`T?`) with flow narrowing.
25. **Error handling** — `try`/`catch` (stack unwinding) or `Result` + `?`.

**Phase D — type-system depth**
26. **Union types** (`int | str`) with narrowing.
27. **Generics** (`fn first<T>(a: [T]): T`).

**Phase E — compiler & runtime polish**
28. **Performance** — inline caching + a bytecode peephole pass + a benchmark.
29. **Native-backend extensions** — closures (and/or simple arrays) lowered to C.

**Recommended companion reading:** *Crafting Interpreters* by Robert Nystrom
(free online). cnano's bytecode/VM design intentionally follows the same lineage
as its "clox", so the book is an excellent deeper dive.

---

## Appendix: extracting cnano into its own git repository

This project is self-contained in the `cnano/` directory. To move it into a
fresh repo of its own:

```bash
# from the parent repo root, with cnano/ committed:
git subtree split --prefix=cnano -b cnano-only      # isolate cnano's history
mkdir ../cnano-repo && cd ../cnano-repo && git init
git pull ../full-stack-blog cnano-only              # import that history
# then add a new remote and push:
# git remote add origin <your-new-repo-url> && git push -u origin main
```

Or simply copy the `cnano/` folder into a new directory and `git init` there if
you do not need the history.

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

## 9. Roadmap: where to go next

Each step below is a self-contained project that teaches a new concept. They are
ordered so each builds on the last.

1. **Booleans, `nil`, and comparisons.** Turn `Value` into a *tagged union*
   (`{ type, union { int64_t i; bool b; } }`). Add `<  >  ==  !=` and `OP_TRUE`,
   `OP_FALSE`. This is the gateway to a real type system and forces you to add
   runtime type checks ("operands must be numbers").
2. **Statements and `print`.** Introduce a statement grammar, a `;` terminator,
   and an `OP_PRINT`. Programs become sequences of statements rather than one
   expression. Add `OP_POP` to discard expression-statement results.
3. **Global variables.** Add an identifier token, `let`/assignment syntax, and
   `OP_DEFINE_GLOBAL` / `OP_GET_GLOBAL` / `OP_SET_GLOBAL` backed by a hash
   table. You will build the hash table — a great data-structures exercise.
4. **Local variables and scope.** Resolve locals to **stack slots** at compile
   time (no hash lookup at runtime). This introduces the *compile-time
   environment* and lexical scoping.
5. **Control flow.** `if`/`else`, `while`, `for` via **jump instructions**
   (`OP_JUMP`, `OP_JUMP_IF_FALSE`) and *backpatching* — emitting a jump with a
   placeholder offset and filling it in once you know the target.
6. **Functions and a call stack.** Call frames, parameters, return values, and a
   real *calling convention*. This is the conceptual heart of any language.
7. **A type checker.** A separate pass over the AST that assigns and verifies
   types *before* running — your first taste of static analysis and the
   static-vs-dynamic trade-off.
8. **Optimisations.** Constant folding on the AST (`2 + 3` → `5` at compile
   time); a Pratt parser; `OP_CONSTANT_LONG`; run-length-encoded line info.
9. **Toward native code.** Emit textual assembly or LLVM IR instead of bytecode,
   and you have crossed from "interpreted" to "compiled". This is the big leap to
   a true low-level, ahead-of-time language.

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

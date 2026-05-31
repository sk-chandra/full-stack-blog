#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"
#include "object.h"

// --- compile-time scope tracking -------------------------------------------
//
// This is the heart of step 4. GLOBALS are stored by name in a runtime hash
// table. LOCALS are different: at runtime they are just values sitting on the VM
// stack, and the compiler assigns each one a fixed SLOT INDEX into that stack.
// To do that, the compiler keeps its own little model of the stack at compile
// time — the `locals` array below — recording which name lives in which slot and
// at what block depth. Resolving a local name is then a search of this array,
// done ONCE at compile time, producing a numeric slot the VM accesses directly.
// No names, no hashing, no lookup at runtime: that is why locals are fast.
//
// Step 6 generalises this: there is now ONE CompilerState per function being
// compiled (the top-level script is itself a function). Each has its own locals
// array, and the `enclosing` pointer links a nested function's compiler back to
// the one around it — a stack of compilers mirroring the nesting of `fn`s.

#define MAX_LOCALS 256 // one byte of slot index -> at most 256 locals in scope

typedef struct {
  ObjString *name; // the local's name (interned, compared by pointer)
  int depth;       // block nesting depth where it was declared; -1 = "declared
                   // but not yet initialised" (see declareLocal/markInitialized)
} Local;

// Whether we are compiling a real function or the implicit top-level script.
// The distinction matters for `return` (illegal at top level) and for what the
// final OP_RETURN does.
typedef enum {
  TYPE_FUNCTION,
  TYPE_SCRIPT,
} FunctionType;

typedef struct CompilerState {
  struct CompilerState *enclosing; // the compiler for the surrounding function
  ObjFunction *function;           // the function this compiler is building
  FunctionType type;

  Local locals[MAX_LOCALS]; // a compile-time mirror of THIS function's slots
  int localCount;           // how many locals are currently in scope
  int scopeDepth;           // current block nesting within this function
} CompilerState;

static CompilerState *current;

// The chunk we are currently emitting into is always the current function's own
// chunk. Making this a function (not a stored pointer) means switching functions
// just needs `current` updated — the chunk follows automatically.
static Chunk *currentChunk(void) { return &current->function->chunk; }

// The compiler can now fail (too many locals, redeclaration, self-reference in an
// initialiser, return at top level). Rather than exit() mid-compile, we record an
// error and let the driver abort cleanly — like the parser's report-and-recover.
static bool hadCompileError;

static void compileError(int line, const char *message) {
  fprintf(stderr, "[line %d] Compile error: %s\n", line, message);
  hadCompileError = true;
}

static void initCompilerState(CompilerState *state, FunctionType type) {
  state->enclosing = current; // link to the surrounding compiler (NULL at top)
  state->function = NULL;
  state->type = type;
  state->localCount = 0;
  state->scopeDepth = 0;
  state->function = newFunction(); // allocate the function being compiled
  current = state;

  // Slot 0 of every function's stack window is reserved for the function being
  // called itself (the callee sits just below its arguments at runtime). We
  // claim it with an unnamed, already-initialised local so user locals start at
  // slot 1 and the indices line up with the runtime frame.
  Local *local = &current->locals[current->localCount++];
  local->depth = 0;
  local->name = NULL;
}

// --- scope management ------------------------------------------------------

// Enter a new block scope: just bump the depth. No runtime cost — locals are
// still being placed on the same stack; depth only tells us, at end of scope,
// which ones to remove.
static void beginScope(void) { current->scopeDepth++; }

// Resolve a name to a local stack slot, or -1 if it is not a local (and is
// therefore a global). We search from the INNERMOST local outward so that an
// inner declaration SHADOWS an outer one with the same name — the first match
// walking backwards is the one in the nearest enclosing scope.
static int resolveLocal(ObjString *name, int line) {
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local *local = &current->locals[i];
    if (local->name == name) { // interned: pointer equality is enough
      if (local->depth == -1)
        compileError(line, "cannot read local variable in its own initialiser");
      return i;
    }
  }
  return -1; // not found among locals -> treat as a global
}

static void emitByte(uint8_t byte, int line) {
  writeChunk(currentChunk(), byte, line);
}

// Emit OP_CONSTANT followed by the index of `value` in the constant pool.
static void emitConstant(Value value, int line) {
  int index = addConstant(currentChunk(), value);
  if (index > 255) {
    // Our OP_CONSTANT operand is a single byte, so it can address only 256
    // constants. Real VMs add an OP_CONSTANT_LONG with a wider operand; we just
    // report the limit. This is a concrete example of an ISA design trade-off:
    // small operands keep bytecode compact but cap how much they can address.
    fprintf(stderr, "[line %d] Error: too many constants in one chunk.\n", line);
    exit(65); // 65 = EX_DATAERR, "input data was incorrect"
  }
  emitByte(OP_CONSTANT, line);
  emitByte((uint8_t)index, line);
}

// --- jumps and backpatching ------------------------------------------------
//
// A forward jump (used by `if` and short-circuit `and`/`or`) has a problem: when
// we emit it, we do NOT yet know how far to jump, because we have not compiled
// the code we want to skip. The standard solution is BACKPATCHING: emit the jump
// with a placeholder offset, remember where the placeholder is, compile the
// skipped code, then go back and overwrite the placeholder with the real
// distance. emitJump returns the placeholder's location; patchJump fills it in.

// Emit `instruction` (OP_JUMP or OP_JUMP_IF_FALSE) followed by a 2-byte
// placeholder offset. Returns the offset of the placeholder so patchJump can
// find it later.
static int emitJump(uint8_t instruction, int line) {
  emitByte(instruction, line);
  emitByte(0xff, line); // placeholder high byte
  emitByte(0xff, line); // placeholder low byte
  return currentChunk()->count - 2;
}

// Backpatch the jump whose 2-byte operand starts at `offset`: compute the
// distance from just after the operand to the CURRENT end of the chunk (the jump
// target), and write it big-endian over the placeholder.
static void patchJump(int offset) {
  // -2 adjusts for the two operand bytes themselves.
  int jump = currentChunk()->count - offset - 2;
  if (jump > UINT16_MAX) {
    // Our offset is 16 bits, so a single jump can't span more than 65535 bytes.
    fprintf(stderr, "[compiler] too much code to jump over\n");
    hadCompileError = true;
  }
  currentChunk()->code[offset] = (jump >> 8) & 0xff; // high byte
  currentChunk()->code[offset + 1] = jump & 0xff;    // low byte
}

// Emit a BACKWARD jump (OP_LOOP) to `loopStart`. Loops are different from
// forward jumps: we already know the target (it's behind us), so no
// backpatching is needed — we compute the distance immediately.
static void emitLoop(int loopStart, int line) {
  emitByte(OP_LOOP, line);
  int offset = currentChunk()->count - loopStart + 2; // +2 for this operand
  if (offset > UINT16_MAX) {
    fprintf(stderr, "[compiler] loop body too large\n");
    hadCompileError = true;
  }
  emitByte((offset >> 8) & 0xff, line);
  emitByte(offset & 0xff, line);
}

// Leave a block scope. Every local declared inside it must be REMOVED from the
// runtime stack, because those slots are about to go out of scope. We emit one
// OP_POP per local and shrink our compile-time model to match. This is the
// runtime cost of a block: proportional to the locals it declared. (Real VMs add
// an OP_POPN that pops several at once; we keep it explicit for clarity.)
static void endScope(int line) {
  current->scopeDepth--;
  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth > current->scopeDepth) {
    emitByte(OP_POP, line);
    current->localCount--;
  }
}

// Record a new local in the compile-time model. Its stack slot is implicitly its
// index in the array (which equals its position on the runtime stack). We start
// it at depth -1 ("uninitialised") so its own initialiser can't refer to it.
static void addLocal(ObjString *name, int line) {
  if (current->localCount == MAX_LOCALS) {
    compileError(line, "too many local variables in scope");
    return;
  }
  Local *local = &current->locals[current->localCount++];
  local->name = name;
  local->depth = -1;
}

// Declare a local for `let` inside a scope. Besides adding it, we forbid
// declaring the SAME name twice in the SAME scope (a likely bug), while still
// allowing an inner scope to shadow an outer one.
static void declareLocal(ObjString *name, int line) {
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local *local = &current->locals[i];
    if (local->depth != -1 && local->depth < current->scopeDepth)
      break; // reached an enclosing scope; shadowing it is fine
    if (local->name == name) {
      compileError(line, "a variable with this name already exists in this scope");
      return;
    }
  }
  addLocal(name, line);
}

// Mark the most-recently-declared local as initialised (depth set to the current
// scope), making it visible to later code. Called AFTER its initialiser compiles.
static void markInitialized(void) {
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}

// Blocks make statement compilation recursive (a block contains statements),
// so emitStatement needs a forward declaration. compileFunction is mutually
// recursive with emitStatement (a function body contains statements; a statement
// may be a nested function).
static void emitStatement(Node *node);
static ObjFunction *compileFunction(Node *node);

// Compile an EXPRESSION node. The contract: every path through here leaves
// exactly ONE value on the VM stack. That invariant is what lets statements
// reason simply about cleanup (print pops one; an expr statement pops one).
static void emitExpr(Node *node) {
  switch (node->type) {
  case NODE_INT:
    // Integers go through the constant pool. We wrap the raw int into a tagged
    // Value right here — the AST stayed representation-agnostic, the compiler
    // bridges to the runtime type.
    emitConstant(INT_VAL(node->as.intValue), node->line);
    break;

  case NODE_BOOL:
    // No constant-pool slot needed: a single opcode encodes the whole value.
    emitByte(node->as.boolValue ? OP_TRUE : OP_FALSE, node->line);
    break;

  case NODE_NIL:
    emitByte(OP_NIL, node->line);
    break;

  case NODE_STRING:
    // A string literal is a heap object; it rides in the constant pool just like
    // an integer, wrapped as an OBJ_VAL.
    emitConstant(OBJ_VAL(node->as.stringValue), node->line);
    break;

  case NODE_VAR_GET: {
    // Resolve the name to a local slot first. If it is a local, emit a fast
    // slot-indexed GET_LOCAL; otherwise fall back to a by-name GET_GLOBAL. This
    // single decision — made here, at compile time — is what separates the two
    // kinds of variable.
    int slot = resolveLocal(node->as.name, node->line);
    if (slot != -1) {
      emitByte(OP_GET_LOCAL, node->line);
      emitByte((uint8_t)slot, node->line);
    } else {
      int nameIdx = addConstant(currentChunk(), OBJ_VAL(node->as.name));
      emitByte(OP_GET_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
    break;
  }

  case NODE_ASSIGN: {
    // Evaluate the value first (it must be on the stack), then store it. As with
    // reads, a local resolves to a slot; otherwise it's a global by name. Neither
    // store pops, so assignment stays an expression: `print x = 5;` prints 5.
    emitExpr(node->as.var.value);
    int slot = resolveLocal(node->as.var.name, node->line);
    if (slot != -1) {
      emitByte(OP_SET_LOCAL, node->line);
      emitByte((uint8_t)slot, node->line);
    } else {
      int nameIdx = addConstant(currentChunk(), OBJ_VAL(node->as.var.name));
      emitByte(OP_SET_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
    break;
  }

  case NODE_LOGICAL: {
    // Short-circuit evaluation, built from jumps. The trick is that
    // OP_JUMP_IF_FALSE peeks WITHOUT popping, so the condition value can double
    // as the result when we short-circuit.
    emitExpr(node->as.logical.left);
    if (node->as.logical.isAnd) {
      // `a and b`: if a is falsey, the whole thing is a — skip b, leaving a.
      // Otherwise pop a and evaluate b, leaving b.
      int endJump = emitJump(OP_JUMP_IF_FALSE, node->line);
      emitByte(OP_POP, node->line); // discard a; b becomes the result
      emitExpr(node->as.logical.right);
      patchJump(endJump);
    } else {
      // `a or b`: if a is falsey, evaluate b; if a is truthy, skip b leaving a.
      // We express this with the same one conditional jump plus an unconditional
      // one: jump-if-false over a small jump that skips b.
      int elseJump = emitJump(OP_JUMP_IF_FALSE, node->line);
      int endJump = emitJump(OP_JUMP, node->line);
      patchJump(elseJump);
      emitByte(OP_POP, node->line); // discard a; b becomes the result
      emitExpr(node->as.logical.right);
      patchJump(endJump);
    }
    break;
  }

  case NODE_CALL: {
    // The calling convention, compiler side: push the callee, then each argument
    // left to right. At runtime the stack is [.. callee arg0 arg1 .. argN]. OP_CALL
    // carries the argument count so the VM knows where the callee sits relative to
    // the top, and can set up a frame whose slot 0 is the callee.
    emitExpr(node->as.call.callee);
    for (int i = 0; i < node->as.call.argCount; i++)
      emitExpr(node->as.call.args[i]);
    emitByte(OP_CALL, node->line);
    emitByte((uint8_t)node->as.call.argCount, node->line);
    break;
  }

  case NODE_UNARY:
    emitExpr(node->as.unary.operand); // operand value now on stack
    switch (node->as.unary.op) {
    case OP_NODE_NEGATE:
      emitByte(OP_NEGATE, node->line);
      break;
    case OP_NODE_NOT:
      emitByte(OP_NOT, node->line);
      break;
    default:
      break; // unreachable
    }
    break;

  case NODE_BINARY:
    // Order matters: left first, then right, so the stack holds [.. left right]
    // and the binary op can pop right then left.
    emitExpr(node->as.binary.left);
    emitExpr(node->as.binary.right);
    switch (node->as.binary.op) {
    case OP_NODE_ADD:
      emitByte(OP_ADD, node->line);
      break;
    case OP_NODE_SUB:
      emitByte(OP_SUB, node->line);
      break;
    case OP_NODE_MUL:
      emitByte(OP_MUL, node->line);
      break;
    case OP_NODE_DIV:
      emitByte(OP_DIV, node->line);
      break;
    case OP_NODE_EQUAL:
      emitByte(OP_EQUAL, node->line);
      break;
    case OP_NODE_LESS:
      emitByte(OP_LESS, node->line);
      break;
    case OP_NODE_GREATER:
      emitByte(OP_GREATER, node->line);
      break;
    default:
      break; // unreachable
    }
    break;

  case NODE_PRINT:
  case NODE_EXPR_STMT:
  case NODE_VAR_DECL:
  case NODE_BLOCK:
  case NODE_IF:
  case NODE_WHILE:
  case NODE_FUN:
  case NODE_RETURN:
    // Statement nodes are not expressions and must never be compiled as one.
    // This case exists only to keep the switch exhaustive (so -Wall warns if a
    // future node type is forgotten).
    break;
  }
}

// Compile a STATEMENT node. The contract here is the OPPOSITE of emitExpr's:
// a statement must leave the stack at the SAME height it started — it produces
// no value. Each kind achieves that by consuming the one value its inner
// expression pushed.
static void emitStatement(Node *node) {
  switch (node->type) {
  case NODE_PRINT:
    emitExpr(node->as.stmt.expr); // value on stack ...
    emitByte(OP_PRINT, node->line); // ... OP_PRINT pops and prints it
    break;

  case NODE_EXPR_STMT:
    emitExpr(node->as.stmt.expr); // value on stack ...
    emitByte(OP_POP, node->line);   // ... discarded, because a statement's
    // value is unused. Forgetting this OP_POP is the classic bug that makes a
    // stack-based VM's stack grow without bound — every expression statement
    // would leak one slot. The pop is what keeps the stack balanced.
    break;

  case NODE_VAR_DECL: {
    ObjString *name = node->as.var.name;
    if (current->scopeDepth > 0) {
      // LOCAL declaration. Declare the name FIRST (marking it uninitialised), so
      // its initialiser cannot legally refer to itself. Then compile the
      // initialiser — its value lands on the stack at exactly this local's slot,
      // and we simply LEAVE it there: no opcode needed to "store" a local, the
      // value's stack position IS the variable. Finally mark it initialised.
      declareLocal(name, node->line);
      emitExpr(node->as.var.value);
      markInitialized();
    } else {
      // GLOBAL declaration, exactly as before: evaluate then DEFINE_GLOBAL pops.
      emitExpr(node->as.var.value);
      int nameIdx = addConstant(currentChunk(), OBJ_VAL(name));
      emitByte(OP_DEFINE_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
    break;
  }

  case NODE_BLOCK: {
    // A block opens a scope, compiles its statements, then closes the scope
    // (which pops its locals). The scope bracketing is the entire mechanism of
    // lexical scoping.
    beginScope();
    Program *body = node->as.block;
    for (int i = 0; i < body->count; i++)
      emitStatement(body->statements[i]);
    endScope(node->line);
    break;
  }

  case NODE_IF: {
    // Layout we emit:
    //     <condition>
    //     JUMP_IF_FALSE  -> else        (thenJump)
    //     POP            (discard the condition value on the THEN path)
    //     <then branch>
    //     JUMP           -> end         (elseJump; skips the else)
    //   else:
    //     POP            (discard the condition value on the ELSE path)
    //     <else branch>                 (empty if no else)
    //   end:
    // The two POPs matter: JUMP_IF_FALSE deliberately does NOT pop, so each path
    // must discard the condition itself — keeping the stack balanced either way.
    emitExpr(node->as.ifStmt.condition);
    int thenJump = emitJump(OP_JUMP_IF_FALSE, node->line);
    emitByte(OP_POP, node->line); // then-path: pop condition
    emitStatement(node->as.ifStmt.then);
    int elseJump = emitJump(OP_JUMP, node->line);

    patchJump(thenJump);          // JUMP_IF_FALSE lands here
    emitByte(OP_POP, node->line); // else-path: pop condition
    if (node->as.ifStmt.otherwise != NULL)
      emitStatement(node->as.ifStmt.otherwise);
    patchJump(elseJump);          // both paths converge here
    break;
  }

  case NODE_WHILE: {
    // Layout we emit:
    //   loopStart:
    //     <condition>
    //     JUMP_IF_FALSE  -> end         (exitJump)
    //     POP            (discard condition; we're entering the body)
    //     <body>
    //     LOOP           -> loopStart   (backward jump; re-test the condition)
    //   end:
    //     POP            (discard condition on the exit path)
    // The backward LOOP is what makes it a loop. Note the symmetric POPs again,
    // for the same reason as `if`.
    int loopStart = currentChunk()->count; // the condition is re-evaluated here
    emitExpr(node->as.whileStmt.condition);
    int exitJump = emitJump(OP_JUMP_IF_FALSE, node->line);
    emitByte(OP_POP, node->line); // enter body: pop condition
    emitStatement(node->as.whileStmt.body);
    emitLoop(loopStart, node->line);

    patchJump(exitJump);
    emitByte(OP_POP, node->line); // exit: pop condition
    break;
  }

  case NODE_FUN: {
    // Compile a function declaration. The resulting ObjFunction is stored as a
    // CONSTANT and pushed with OP_CONSTANT, then bound to its name exactly like a
    // variable (global at top level, local inside a block). Binding the name
    // BEFORE compiling the body would also allow self-recursion for locals; for
    // globals it doesn't matter because globals are looked up by name at runtime,
    // so a function can always call itself and peers.
    ObjFunction *function = compileFunction(node);
    if (function == NULL)
      break; // a compile error occurred inside the body; keep going
    emitConstant(OBJ_VAL(function), node->line);

    ObjString *name = node->as.fun.name;
    if (current->scopeDepth > 0) {
      // Local function: it now sits on the stack at the next slot. Register it as
      // an initialised local — no store opcode needed (its stack slot is it).
      declareLocal(name, node->line);
      markInitialized();
    } else {
      int nameIdx = addConstant(currentChunk(), OBJ_VAL(name));
      emitByte(OP_DEFINE_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
    break;
  }

  case NODE_RETURN: {
    // `return` is only legal inside a function, not in the top-level script.
    if (current->type == TYPE_SCRIPT) {
      compileError(node->line, "can't return from top-level code");
      break;
    }
    if (node->as.ret.value != NULL) {
      emitExpr(node->as.ret.value); // value on top ...
    } else {
      emitByte(OP_NIL, node->line); // bare `return;` returns nil
    }
    emitByte(OP_RETURN, node->line); // ... OP_RETURN hands it back to the caller
    break;
  }

  default:
    // An expression appearing where a statement is expected: shouldn't happen,
    // the parser only ever produces statement nodes at the top level.
    break;
  }
}

// Compile one function declaration into a fresh ObjFunction. This spins up a NEW
// CompilerState (linked to the current one via `enclosing`), so the function gets
// its own chunk and its own locals/slot numbering starting fresh — exactly the
// isolation a separate stack frame provides at runtime. Parameters are just the
// function's first locals.
static ObjFunction *compileFunction(Node *node) {
  CompilerState state;
  initCompilerState(&state, TYPE_FUNCTION);
  current->function->name = node->as.fun.name;
  current->function->arity = node->as.fun.paramCount;

  beginScope(); // the function body is its own scope

  // Declare each parameter as a local. At runtime the caller will have placed the
  // arguments in exactly these slots, so parameters ARE locals 1..arity.
  for (int i = 0; i < node->as.fun.paramCount; i++) {
    declareLocal(node->as.fun.params[i], node->line);
    markInitialized();
  }

  // Compile the body statements.
  Program *body = node->as.fun.body;
  for (int i = 0; i < body->count; i++)
    emitStatement(body->statements[i]);

  // Every function ends with an implicit `return nil;` so falling off the end is
  // well-defined. (We don't bother closing the scope with POPs — OP_RETURN tears
  // down the whole frame at once.)
  emitByte(OP_NIL, node->line);
  emitByte(OP_RETURN, node->line);

  ObjFunction *function = current->function;
  // Pop this compiler off the stack, restoring the enclosing one.
  current = current->enclosing;
  return function;
}

ObjFunction *compile(Program *program) {
  CompilerState state;
  initCompilerState(&state, TYPE_SCRIPT); // the top level is an implicit function
  hadCompileError = false;

  // Emit each top-level statement in source order. Because every statement is
  // stack-neutral, the stack is empty between statements — exactly as a
  // sequence of independent actions should behave.
  for (int i = 0; i < program->count; i++)
    emitStatement(program->statements[i]);
  // The script ends with an implicit `return nil;` — the VM runs the top level as
  // a function call, so it returns just like any other.
  int lastLine = program->count > 0
                     ? program->statements[program->count - 1]->line
                     : 1;
  emitByte(OP_NIL, lastLine);
  emitByte(OP_RETURN, lastLine);

  ObjFunction *function = current->function;
  current = NULL;
  return hadCompileError ? NULL : function;
}

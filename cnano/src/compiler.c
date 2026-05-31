#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler.h"

// We thread the target chunk through a file-static pointer to keep the recursive
// helper signatures small. Single-threaded CLI, so this is safe.
static Chunk *currentChunk;

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

#define MAX_LOCALS 256 // one byte of slot index -> at most 256 locals in scope

typedef struct {
  ObjString *name; // the local's name (interned, compared by pointer)
  int depth;       // block nesting depth where it was declared; -1 = "declared
                   // but not yet initialised" (see declareLocal/markInitialized)
} Local;

typedef struct {
  Local locals[MAX_LOCALS]; // a compile-time mirror of the runtime stack slots
  int localCount;           // how many locals are currently in scope
  int scopeDepth;           // current block nesting: 0 = global, 1 = first {}, …
} CompilerState;

static CompilerState *current;

static void initCompilerState(CompilerState *state) {
  state->localCount = 0;
  state->scopeDepth = 0;
  current = state;
}

// The compiler can now fail (too many locals, redeclaration, self-reference in an
// initialiser). Rather than exit() mid-compile, we record an error and let the
// driver abort cleanly — mirroring how the parser already reports and recovers.
static bool hadCompileError;

static void compileError(int line, const char *message) {
  fprintf(stderr, "[line %d] Compile error: %s\n", line, message);
  hadCompileError = true;
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
  writeChunk(currentChunk, byte, line);
}

// Emit OP_CONSTANT followed by the index of `value` in the constant pool.
static void emitConstant(Value value, int line) {
  int index = addConstant(currentChunk, value);
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
// so emitStatement needs a forward declaration.
static void emitStatement(Node *node);

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
      int nameIdx = addConstant(currentChunk, OBJ_VAL(node->as.name));
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
      int nameIdx = addConstant(currentChunk, OBJ_VAL(node->as.var.name));
      emitByte(OP_SET_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
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
      int nameIdx = addConstant(currentChunk, OBJ_VAL(name));
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

  default:
    // An expression appearing where a statement is expected: shouldn't happen,
    // the parser only ever produces statement nodes at the top level.
    break;
  }
}

bool compile(Program *program, Chunk *chunk) {
  currentChunk = chunk;
  CompilerState state;
  initCompilerState(&state); // start at global scope with no locals
  hadCompileError = false;

  // Emit each top-level statement in source order. Because every statement is
  // stack-neutral, the stack is empty between statements — exactly as a
  // sequence of independent actions should behave.
  for (int i = 0; i < program->count; i++)
    emitStatement(program->statements[i]);
  // A final OP_RETURN marks the end of the program. It no longer carries a
  // result value (programs communicate via `print` now), so it just halts.
  int lastLine = program->count > 0
                     ? program->statements[program->count - 1]->line
                     : 1;
  emitByte(OP_RETURN, lastLine);

  return !hadCompileError;
}

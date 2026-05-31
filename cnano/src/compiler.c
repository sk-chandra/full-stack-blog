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
  bool isCaptured; // does a nested closure capture this local? If so, when it
                   // leaves scope we must CLOSE it (move to heap) not just pop it.
} Local;

// One captured variable, from the compiler's point of view. `isLocal` says
// whether the capture refers to a LOCAL of the immediately-enclosing function
// (capture it directly from that frame's slot) or to an UPVALUE of the enclosing
// function (chain through — the variable is two or more levels up). `index` is
// the slot or upvalue index in that enclosing function.
typedef struct {
  uint8_t index;
  bool isLocal;
} Upvalue;

// Whether we are compiling a real function or the implicit top-level script.
// The distinction matters for `return` (illegal at top level) and for what the
// final OP_RETURN does.
typedef enum {
  TYPE_FUNCTION,
  TYPE_METHOD,      // a struct method: slot 0 is the receiver, `self`
  TYPE_INITIALIZER, // a method named `init`: implicitly returns `self`
  TYPE_SCRIPT,
} FunctionType;

typedef struct CompilerState {
  struct CompilerState *enclosing; // the compiler for the surrounding function
  ObjFunction *function;           // the function this compiler is building
  FunctionType type;

  Local locals[MAX_LOCALS]; // a compile-time mirror of THIS function's slots
  int localCount;           // how many locals are currently in scope
  Upvalue upvalues[MAX_LOCALS]; // the variables THIS function captures
  int scopeDepth;           // current block nesting within this function
} CompilerState;

static CompilerState *current;

// A `break`/`continue` target. Loops nest, so these form a stack threaded through
// `currentLoop`. Each records the jumps to back-patch and the scope depth at the
// loop, so break/continue know how many body locals to discard before jumping.
#define MAX_LOOP_EXITS 256
typedef struct Loop {
  struct Loop *enclosing;
  int scopeDepth;
  int breakJumps[MAX_LOOP_EXITS];
  int breakCount;
  int continueJumps[MAX_LOOP_EXITS];
  int continueCount;
} Loop;

static Loop *currentLoop = NULL;

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
  // For a METHOD, slot 0 holds the receiver: name it `self` so the body can refer
  // to it as an ordinary local. For plain functions slot 0 is the (unnamed)
  // callee. Interning makes the body's `self` reads resolve here by pointer.
  local->name = (type == TYPE_METHOD || type == TYPE_INITIALIZER)
                    ? copyString("self", 4)
                    : NULL;
  local->isCaptured = false;
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
// Resolve in a SPECIFIC compiler's locals (not necessarily the current one), so
// upvalue resolution can look into enclosing functions. Returns the slot or -1.
static int resolveLocalIn(CompilerState *compiler, ObjString *name, int line) {
  for (int i = compiler->localCount - 1; i >= 0; i--) {
    Local *local = &compiler->locals[i];
    if (local->name == name) { // interned: pointer equality is enough
      if (local->depth == -1)
        compileError(line, "cannot read local variable in its own initialiser");
      return i;
    }
  }
  return -1; // not found among this function's locals
}

static int resolveLocal(ObjString *name, int line) {
  return resolveLocalIn(current, name, line);
}

// Record that `compiler`'s function captures an upvalue described by
// (index, isLocal), returning its index in that function's upvalue array. We
// DEDUPE: if the same upvalue was already captured, reuse it, so each captured
// variable has exactly one upvalue per closure (essential for shared mutation).
static int addUpvalue(CompilerState *compiler, uint8_t index, bool isLocal,
                      int line) {
  int count = compiler->function->upvalueCount;
  for (int i = 0; i < count; i++) {
    Upvalue *uv = &compiler->upvalues[i];
    if (uv->index == index && uv->isLocal == isLocal)
      return i; // already captured — reuse
  }
  if (count == MAX_LOCALS) {
    compileError(line, "too many captured variables (upvalues) in function");
    return 0;
  }
  compiler->upvalues[count].isLocal = isLocal;
  compiler->upvalues[count].index = index;
  return compiler->function->upvalueCount++;
}

// THE recursive heart of closures. Try to resolve `name` as an upvalue of
// `compiler` (i.e. a variable owned by some ENCLOSING function). Returns an
// upvalue index, or -1 if the name is not found in any enclosing function (so it
// must be a global).
//
// The recursion has two cases:
//   1. The name is a LOCAL of the immediately-enclosing function: mark that
//      local as captured and add an upvalue that points straight at its slot.
//   2. Otherwise, recurse: ask the enclosing function to resolve it as one of
//      ITS upvalues. If that succeeds, add an upvalue that chains to the
//      enclosing upvalue. This "chaining" is what lets a closure capture a
//      variable from two or more levels up — each intervening function passes it
//      along, hop by hop.
static int resolveUpvalue(CompilerState *compiler, ObjString *name, int line) {
  if (compiler->enclosing == NULL)
    return -1; // reached the top level: not an upvalue, must be a global

  int local = resolveLocalIn(compiler->enclosing, name, line);
  if (local != -1) {
    compiler->enclosing->locals[local].isCaptured = true; // mark for closing
    return addUpvalue(compiler, (uint8_t)local, /*isLocal=*/true, line);
  }

  int upvalue = resolveUpvalue(compiler->enclosing, name, line);
  if (upvalue != -1)
    return addUpvalue(compiler, (uint8_t)upvalue, /*isLocal=*/false, line);

  return -1;
}

static void emitByte(uint8_t byte, int line) {
  writeChunk(currentChunk(), byte, line);
}

// Add `value` to the current chunk's constant pool, REUSING an existing slot if
// an equal constant is already there. Deduplication shrinks the pool (a variable
// name or repeated literal occupies one slot no matter how often it appears) —
// both a small optimisation and what keeps name operands inside one byte far
// longer. Returns the constant index.
static int makeConstant(Value value) {
  Chunk *chunk = currentChunk();
  for (int i = 0; i < chunk->constants.count; i++) {
    // STRICT dedup: same tag AND equal. valuesEqual treats int and float
    // numerically (1 == 1.0), but a constant slot stores one representation, so
    // we must not merge an int constant with a float one.
    Value c = chunk->constants.values[i];
    if (c.type == value.type && valuesEqual(c, value))
      return i;
  }
  return addConstant(chunk, value);
}

// Add a variable-NAME constant (deduped) for the by-name global opcodes, which
// carry a single-byte index. If the pool grows past 255 we report a clean
// compile error rather than letting the byte wrap and read the wrong name.
static int identifierConstant(ObjString *name, int line) {
  int index = makeConstant(OBJ_VAL(name));
  if (index > 0xff) {
    compileError(line, "too many named globals in one function (>256 constants)");
    return 0;
  }
  return index;
}

// Push a constant. Uses the compact 1-byte OP_CONSTANT when the index fits in a
// byte, and falls back to the 3-byte OP_CONSTANT_LONG otherwise — so a chunk is
// no longer capped at 256 constants, while everyday code stays small.
static void emitConstant(Value value, int line) {
  int index = makeConstant(value);
  if (index <= 0xff) {
    emitByte(OP_CONSTANT, line);
    emitByte((uint8_t)index, line);
  } else if (index <= 0xffffff) {
    emitByte(OP_CONSTANT_LONG, line);
    emitByte((uint8_t)((index >> 16) & 0xff), line);
    emitByte((uint8_t)((index >> 8) & 0xff), line);
    emitByte((uint8_t)(index & 0xff), line);
  } else {
    // 2^24 constants in one function is astronomically unlikely; still, fail
    // cleanly rather than silently truncating.
    compileError(line, "too many constants in one function");
  }
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
// runtime stack, because those slots are about to go out of scope. For a normal
// local we emit OP_POP. But if a local was CAPTURED by a closure, popping it
// would lose a variable the closure still needs — so instead we emit
// OP_CLOSE_UPVALUE, which lifts the value off the stack onto the heap before
// discarding the slot. That is the moment a captured variable's lifetime is
// extended beyond its frame: the essence of a closure.
static void endScope(int line) {
  current->scopeDepth--;
  while (current->localCount > 0 &&
         current->locals[current->localCount - 1].depth > current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured)
      emitByte(OP_CLOSE_UPVALUE, line);
    else
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
  local->isCaptured = false; // becomes true if a nested closure captures it
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
static ObjFunction *compileFunction(Node *node, FunctionType type);

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

  case NODE_FLOAT:
    emitConstant(FLOAT_VAL(node->as.floatValue), node->line);
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
    // Three-way resolution, in order of nearness: a LOCAL of this function, an
    // UPVALUE captured from an enclosing function, or a GLOBAL by name. The
    // compiler decides which once, here — the VM never searches by name except
    // for true globals.
    int arg = resolveLocal(node->as.name, node->line);
    if (arg != -1) {
      emitByte(OP_GET_LOCAL, node->line);
      emitByte((uint8_t)arg, node->line);
    } else if ((arg = resolveUpvalue(current, node->as.name, node->line)) != -1) {
      emitByte(OP_GET_UPVALUE, node->line);
      emitByte((uint8_t)arg, node->line);
    } else {
      int nameIdx = identifierConstant(node->as.name, node->line);
      emitByte(OP_GET_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
    break;
  }

  case NODE_ASSIGN: {
    // Evaluate the value first (it must be on the stack), then store it. Same
    // three-way resolution as reads. None of the stores pop, so assignment stays
    // an expression: `print x = 5;` prints 5.
    emitExpr(node->as.var.value);
    int arg = resolveLocal(node->as.var.name, node->line);
    if (arg != -1) {
      emitByte(OP_SET_LOCAL, node->line);
      emitByte((uint8_t)arg, node->line);
    } else if ((arg = resolveUpvalue(current, node->as.var.name, node->line)) !=
               -1) {
      emitByte(OP_SET_UPVALUE, node->line);
      emitByte((uint8_t)arg, node->line);
    } else {
      int nameIdx = identifierConstant(node->as.var.name, node->line);
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

  case NODE_COND: {
    // `c ? a : b` — like an `if` that yields a value. We must leave EXACTLY one
    // value on the stack (either a or b). OP_JUMP_IF_FALSE peeks without popping,
    // so each branch pops the condition itself before pushing its result.
    emitExpr(node->as.ifStmt.condition);
    int elseJump = emitJump(OP_JUMP_IF_FALSE, node->line);
    emitByte(OP_POP, node->line); // discard the condition on the true path
    emitExpr(node->as.ifStmt.then);
    int endJump = emitJump(OP_JUMP, node->line);
    patchJump(elseJump);
    emitByte(OP_POP, node->line); // discard the condition on the false path
    emitExpr(node->as.ifStmt.otherwise);
    patchJump(endJump);
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

  case NODE_IS: {
    emitExpr(node->as.isTest.expr); // the value to test, on the stack
    Type *t = node->as.isTest.type;
    int tag = -1;
    switch (t->kind) {
    case TY_INT: tag = IS_TAG_INT; break;
    case TY_BOOL: tag = IS_TAG_BOOL; break;
    case TY_STR: tag = IS_TAG_STR; break;
    case TY_NIL: tag = IS_TAG_NIL; break;
    case TY_ARRAY: tag = IS_TAG_ARRAY; break;
    case TY_MAP: tag = IS_TAG_MAP; break;
    case TY_STRUCT: {
      // Push the named struct, then test instance-of.
      int nameIdx = identifierConstant(t->strct.name, node->line);
      emitByte(OP_GET_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
      emitByte(OP_IS_STRUCT, node->line);
      break;
    }
    default:
      compileError(node->line,
                   "`is` needs a concrete type (int, bool, str, nil, array, "
                   "map, or a struct name)");
      break;
    }
    if (tag >= 0) {
      emitByte(OP_IS_KIND, node->line);
      emitByte((uint8_t)tag, node->line);
    }
    break;
  }

  case NODE_INVOKE: {
    // Like a call, but the "callee" is a method NAME resolved on the receiver at
    // runtime. Push the receiver, then the arguments, then OP_INVOKE with the
    // method-name constant and the argument count.
    emitExpr(node->as.invoke.receiver);
    for (int i = 0; i < node->as.invoke.argCount; i++)
      emitExpr(node->as.invoke.args[i]);
    int nameIdx = identifierConstant(node->as.invoke.method, node->line);
    emitByte(OP_INVOKE, node->line);
    emitByte((uint8_t)nameIdx, node->line);
    emitByte((uint8_t)node->as.invoke.argCount, node->line);
    break;
  }

  case NODE_ARRAY: {
    // Push each element left-to-right, then build the array from the top `count`.
    for (int i = 0; i < node->as.array.count; i++)
      emitExpr(node->as.array.elements[i]);
    emitByte(OP_BUILD_ARRAY, node->line);
    emitByte((uint8_t)node->as.array.count, node->line);
    break;
  }

  case NODE_MAP: {
    // Push key0, value0, key1, value1, ... then build the map from the top pairs.
    for (int i = 0; i < node->as.map.count; i++) {
      emitExpr(node->as.map.keys[i]);
      emitExpr(node->as.map.values[i]);
    }
    emitByte(OP_BUILD_MAP, node->line);
    emitByte((uint8_t)node->as.map.count, node->line);
    break;
  }

  case NODE_INDEX_GET:
    emitExpr(node->as.index.object); // [.. object]
    emitExpr(node->as.index.index);  // [.. object index]
    emitByte(OP_INDEX_GET, node->line);
    break;

  case NODE_INDEX_SET:
    emitExpr(node->as.index.object); // [.. object]
    emitExpr(node->as.index.index);  // [.. object index]
    emitExpr(node->as.index.value);  // [.. object index value]
    emitByte(OP_INDEX_SET, node->line);
    break;

  case NODE_FIELD_GET: {
    emitExpr(node->as.field.object); // [.. object]
    int nameIdx = identifierConstant(node->as.field.field, node->line);
    emitByte(OP_GET_FIELD, node->line);
    emitByte((uint8_t)nameIdx, node->line);
    break;
  }

  case NODE_FIELD_SET: {
    emitExpr(node->as.field.object); // [.. object]
    emitExpr(node->as.field.value);  // [.. object value]
    int nameIdx = identifierConstant(node->as.field.field, node->line);
    emitByte(OP_SET_FIELD, node->line);
    emitByte((uint8_t)nameIdx, node->line);
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
    case OP_NODE_BITNOT:
      emitByte(OP_BITNOT, node->line);
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
    case OP_NODE_MOD:
      emitByte(OP_MOD, node->line);
      break;
    case OP_NODE_BITAND:
      emitByte(OP_BITAND, node->line);
      break;
    case OP_NODE_BITOR:
      emitByte(OP_BITOR, node->line);
      break;
    case OP_NODE_BITXOR:
      emitByte(OP_BITXOR, node->line);
      break;
    case OP_NODE_SHL:
      emitByte(OP_SHL, node->line);
      break;
    case OP_NODE_SHR:
      emitByte(OP_SHR, node->line);
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

  case NODE_FUN:
    // A lambda: compile the function (emits OP_CLOSURE) and LEAVE the closure on
    // the stack as this expression's value — the only difference from a named
    // function declaration is that we don't bind it to a name afterwards.
    compileFunction(node, TYPE_FUNCTION);
    break;

  case NODE_PRINT:
  case NODE_EXPR_STMT:
  case NODE_VAR_DECL:
  case NODE_BLOCK:
  case NODE_IF:
  case NODE_WHILE:
  case NODE_RETURN:
  case NODE_STRUCT:
  case NODE_THROW:
  case NODE_TRY:
  case NODE_BREAK:
  case NODE_CONTINUE:
  case NODE_IMPORT:
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
      int nameIdx = identifierConstant(name, node->line);
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
    // Layout:
    //   loopStart: <cond> JUMP_IF_FALSE->exit  POP  <body>
    //   continueTarget: [<increment> POP]  LOOP->loopStart
    //   exit: POP   breakTarget:
    // A `continue` jumps to continueTarget (so it still runs the increment); a
    // `break` jumps to breakTarget (past the exit POP). We push a loop context so
    // those statements know where to jump and how many locals to discard.
    Loop loop;
    loop.enclosing = currentLoop;
    loop.scopeDepth = current->scopeDepth; // depth just outside the body
    loop.breakCount = 0;
    loop.continueCount = 0;
    currentLoop = &loop;

    int loopStart = currentChunk()->count; // the condition is re-evaluated here
    emitExpr(node->as.whileStmt.condition);
    int exitJump = emitJump(OP_JUMP_IF_FALSE, node->line);
    emitByte(OP_POP, node->line); // enter body: pop condition
    emitStatement(node->as.whileStmt.body);

    // continue lands here, BEFORE the increment, so it isn't skipped: patchJump
    // targets the current position, which is exactly this point.
    for (int i = 0; i < loop.continueCount; i++)
      patchJump(loop.continueJumps[i]);
    if (node->as.whileStmt.increment != NULL) {
      emitExpr(node->as.whileStmt.increment);
      emitByte(OP_POP, node->line); // the increment is run for effect
    }
    emitLoop(loopStart, node->line);

    patchJump(exitJump);
    emitByte(OP_POP, node->line); // exit: pop condition
    for (int i = 0; i < loop.breakCount; i++)
      patchJump(loop.breakJumps[i]); // -> here, past the loop
    currentLoop = loop.enclosing;
    break;
  }

  case NODE_BREAK:
  case NODE_CONTINUE: {
    if (currentLoop == NULL) {
      compileError(node->line, node->type == NODE_BREAK
                                   ? "'break' is only valid inside a loop"
                                   : "'continue' is only valid inside a loop");
      break;
    }
    // Discard locals declared inside the loop body before jumping out of it, so
    // the operand stack stays balanced (the block's own scope-exit POPs are
    // skipped by the jump).
    for (int i = current->localCount - 1;
         i >= 0 && current->locals[i].depth > currentLoop->scopeDepth; i--)
      emitByte(current->locals[i].isCaptured ? OP_CLOSE_UPVALUE : OP_POP,
               node->line);
    int jump = emitJump(OP_JUMP, node->line);
    if (node->type == NODE_BREAK)
      currentLoop->breakJumps[currentLoop->breakCount++] = jump;
    else
      currentLoop->continueJumps[currentLoop->continueCount++] = jump;
    break;
  }

  case NODE_FUN: {
    // Compile the function, which emits OP_CLOSURE leaving the new closure on the
    // stack. Then bind it to its name exactly like a variable — global at top
    // level, local inside a block.
    compileFunction(node, TYPE_FUNCTION);

    ObjString *name = node->as.fun.name;
    if (current->scopeDepth > 0) {
      // Local function: it now sits on the stack at the next slot. Register it as
      // an initialised local — no store opcode needed (its stack slot is it).
      declareLocal(name, node->line);
      markInitialized();
    } else {
      int nameIdx = identifierConstant(name, node->line);
      emitByte(OP_DEFINE_GLOBAL, node->line);
      emitByte((uint8_t)nameIdx, node->line);
    }
    break;
  }

  case NODE_STRUCT: {
    // Build the runtime struct object now — field names are known at compile
    // time — and ride it in the constant pool, then bind it to its name like any
    // declaration. The AST owns its own field-name array, so copy it (the
    // ObjStruct, freed by the GC, will own this copy). GC is off during
    // compilation, so the fresh object survives until it is a rooted constant.
    int n = node->as.structDecl.fieldCount;
    ObjString **names = NULL;
    if (n > 0) {
      names = malloc(sizeof(ObjString *) * n);
      if (names == NULL) {
        fprintf(stderr, "cnano: out of memory compiling struct\n");
        exit(70);
      }
      for (int i = 0; i < n; i++)
        names[i] = node->as.structDecl.fieldNames[i];
    }
    ObjStruct *s = newStruct(node->as.structDecl.name, names, n);
    emitConstant(OBJ_VAL(s), node->line); // pushes the struct object

    // Define each method: compile it as a closure (TYPE_METHOD reserves slot 0 for
    // `self`), then OP_METHOD pops the closure and installs it on the struct,
    // which stays on the stack throughout.
    for (int i = 0; i < node->as.structDecl.methodCount; i++) {
      Node *m = node->as.structDecl.methods[i];
      // A method literally named `init` is the constructor: compile it so it
      // returns `self`. (copyString interns, so the pointer compare is exact.)
      FunctionType mt = m->as.fun.name == copyString("init", 4) ? TYPE_INITIALIZER
                                                                : TYPE_METHOD;
      compileFunction(m, mt);
      int methodNameIdx = identifierConstant(m->as.fun.name, m->line);
      emitByte(OP_METHOD, m->line);
      emitByte((uint8_t)methodNameIdx, m->line);
    }

    ObjString *name = node->as.structDecl.name;
    if (current->scopeDepth > 0) {
      declareLocal(name, node->line);
      markInitialized();
    } else {
      int nameIdx = identifierConstant(name, node->line);
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
    if (current->type == TYPE_INITIALIZER) {
      // An initializer always yields the new instance, so it cannot return a
      // value; a bare `return;` is allowed and returns `self` (slot 0).
      if (node->as.ret.value != NULL)
        compileError(node->line, "can't return a value from an initializer");
      emitByte(OP_GET_LOCAL, node->line);
      emitByte(0, node->line); // slot 0 == self
      emitByte(OP_RETURN, node->line);
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

  case NODE_THROW:
    emitExpr(node->as.stmt.expr); // the value to raise, on top
    emitByte(OP_THROW, node->line);
    break;

  case NODE_TRY: {
    // OP_BEGIN_TRY registers a handler pointing at the catch code; on the normal
    // path OP_END_TRY pops it and we JUMP over the catch. A throw inside the body
    // unwinds to the catch, where the thrown value is on top — bound as the catch
    // variable (a local at exactly the stack depth the handler restored to).
    int handler = emitJump(OP_BEGIN_TRY, node->line);
    emitStatement(node->as.tryStmt.body); // a block (its own scope)
    emitByte(OP_END_TRY, node->line);
    int over = emitJump(OP_JUMP, node->line);

    patchJump(handler); // OP_BEGIN_TRY's offset lands here, at the catch
    beginScope();
    declareLocal(node->as.tryStmt.catchName, node->line); // = the thrown value
    markInitialized();
    emitStatement(node->as.tryStmt.handler);
    endScope(node->line); // pops the catch variable
    patchJump(over);
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
static ObjFunction *compileFunction(Node *node, FunctionType type) {
  CompilerState state;
  initCompilerState(&state, type);
  Loop *savedLoop = currentLoop; // a loop never spans a function boundary
  currentLoop = NULL;
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

  // Every function ends with an implicit return so falling off the end is
  // well-defined: an initializer returns `self` (slot 0); everything else nil.
  // (We don't bother closing the scope with POPs — OP_RETURN tears the frame
  // down at once.)
  if (type == TYPE_INITIALIZER) {
    emitByte(OP_GET_LOCAL, node->line);
    emitByte(0, node->line);
  } else {
    emitByte(OP_NIL, node->line);
  }
  emitByte(OP_RETURN, node->line);

  ObjFunction *function = current->function;
  // Snapshot the upvalue table BEFORE popping this compiler — we need it to emit
  // the OP_CLOSURE operands into the ENCLOSING function's chunk.
  Upvalue upvalues[MAX_LOCALS];
  int upvalueCount = function->upvalueCount;
  for (int i = 0; i < upvalueCount; i++)
    upvalues[i] = current->upvalues[i];

  // Pop this compiler off the stack, restoring the enclosing one.
  current = current->enclosing;
  currentLoop = savedLoop;

  // Emit OP_CLOSURE in the enclosing chunk: the function constant, then two
  // bytes per upvalue (isLocal, index) telling the VM how to capture each one at
  // closure-creation time. A plain function simply has zero upvalue operands.
  int constIdx = addConstant(currentChunk(), OBJ_VAL(function));
  emitByte(OP_CLOSURE, node->line);
  emitByte((uint8_t)constIdx, node->line);
  for (int i = 0; i < upvalueCount; i++) {
    emitByte(upvalues[i].isLocal ? 1 : 0, node->line);
    emitByte(upvalues[i].index, node->line);
  }

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

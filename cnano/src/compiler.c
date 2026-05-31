#include <stdio.h>
#include <stdlib.h>

#include "compiler.h"

// We thread the target chunk through a file-static pointer to keep the recursive
// helper signatures small. Single-threaded CLI, so this is safe.
static Chunk *current;

static void emitByte(uint8_t byte, int line) {
  writeChunk(current, byte, line);
}

// Emit OP_CONSTANT followed by the index of `value` in the constant pool.
static void emitConstant(Value value, int line) {
  int index = addConstant(current, value);
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

  default:
    // An expression appearing where a statement is expected: shouldn't happen,
    // the parser only ever produces statement nodes at the top level.
    break;
  }
}

void compile(Program *program, Chunk *chunk) {
  current = chunk;
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
}

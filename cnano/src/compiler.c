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

// The core recursive walk. Each case leaves exactly one value on the VM stack.
static void emitNode(Node *node) {
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
    emitNode(node->as.unary.operand); // operand value now on stack
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
    emitNode(node->as.binary.left);
    emitNode(node->as.binary.right);
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
  }
}

void compile(Node *tree, Chunk *chunk) {
  current = chunk;
  emitNode(tree);
  // Every chunk ends with OP_RETURN so the VM knows where execution stops and
  // can hand back the final value sitting on top of the stack.
  emitByte(OP_RETURN, tree->line);
}

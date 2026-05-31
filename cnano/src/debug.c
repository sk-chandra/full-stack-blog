#include <stdio.h>

#include "debug.h"

void disassembleChunk(Chunk *chunk, const char *name) {
  printf("== %s ==\n", name);
  // Instructions are variable width (OP_CONSTANT is 2 bytes, the rest are 1), so
  // we cannot just loop by 1. Each disassemble call returns the next offset.
  for (int offset = 0; offset < chunk->count;) {
    offset = disassembleInstruction(chunk, offset);
  }
}

// Helper for a one-byte instruction: print its name, advance by 1.
static int simpleInstruction(const char *name, int offset) {
  printf("%s\n", name);
  return offset + 1;
}

// Helper for OP_CONSTANT: print its name, the operand index, and the value it
// refers to. Advances by 2 (opcode + operand byte).
static int constantInstruction(const char *name, Chunk *chunk, int offset) {
  uint8_t index = chunk->code[offset + 1];
  printf("%-16s %4d '", name, index);
  printValue(chunk->constants.values[index]);
  printf("'\n");
  return offset + 2;
}

int disassembleInstruction(Chunk *chunk, int offset) {
  printf("%04d ", offset); // byte offset, like an address

  // Print the source line. Use "|" when this instruction shares a line with the
  // previous one, so multi-instruction lines read cleanly.
  if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
    printf("   | ");
  } else {
    printf("%4d ", chunk->lines[offset]);
  }

  uint8_t instruction = chunk->code[offset];
  switch (instruction) {
  case OP_CONSTANT:
    return constantInstruction("OP_CONSTANT", chunk, offset);
  case OP_NEGATE:
    return simpleInstruction("OP_NEGATE", offset);
  case OP_ADD:
    return simpleInstruction("OP_ADD", offset);
  case OP_SUB:
    return simpleInstruction("OP_SUB", offset);
  case OP_MUL:
    return simpleInstruction("OP_MUL", offset);
  case OP_DIV:
    return simpleInstruction("OP_DIV", offset);
  case OP_RETURN:
    return simpleInstruction("OP_RETURN", offset);
  default:
    printf("Unknown opcode %d\n", instruction);
    return offset + 1;
  }
}

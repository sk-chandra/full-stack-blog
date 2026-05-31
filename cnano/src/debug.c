#include <stdio.h>

#include "debug.h"
#include "object.h" // AS_FUNCTION, for disassembling OP_CLOSURE

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

// Helper for an instruction with a one-byte operand that is NOT a constant index
// — e.g. a local-variable stack slot. Prints the raw operand value.
static int byteInstruction(const char *name, Chunk *chunk, int offset) {
  uint8_t slot = chunk->code[offset + 1];
  printf("%-16s %4d\n", name, slot);
  return offset + 2;
}

// Helper for a jump instruction: decode its 2-byte operand and print both the
// instruction's own location and the absolute target it jumps to. `sign` is +1
// for forward jumps and -1 for OP_LOOP, so the arithmetic matches the VM.
static int jumpInstruction(const char *name, int sign, Chunk *chunk,
                           int offset) {
  uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
  jump |= chunk->code[offset + 2];
  printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
  return offset + 3;
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

// Helper for OP_INVOKE: a method-name constant followed by an argument count.
// Advances by 3 (opcode + name index + argc).
static int invokeInstruction(const char *name, Chunk *chunk, int offset) {
  uint8_t index = chunk->code[offset + 1];
  uint8_t argc = chunk->code[offset + 2];
  printf("%-16s (%d args) %4d '", name, argc, index);
  printValue(chunk->constants.values[index]);
  printf("'\n");
  return offset + 3;
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
  case OP_CONSTANT_LONG: {
    // 3-byte big-endian operand.
    int index = (chunk->code[offset + 1] << 16) |
                (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
    printf("%-16s %4d '", "OP_CONSTANT_LONG", index);
    printValue(chunk->constants.values[index]);
    printf("'\n");
    return offset + 4;
  }
  case OP_NIL:
    return simpleInstruction("OP_NIL", offset);
  case OP_TRUE:
    return simpleInstruction("OP_TRUE", offset);
  case OP_FALSE:
    return simpleInstruction("OP_FALSE", offset);
  case OP_NEGATE:
    return simpleInstruction("OP_NEGATE", offset);
  case OP_NOT:
    return simpleInstruction("OP_NOT", offset);
  case OP_ADD:
    return simpleInstruction("OP_ADD", offset);
  case OP_SUB:
    return simpleInstruction("OP_SUB", offset);
  case OP_MUL:
    return simpleInstruction("OP_MUL", offset);
  case OP_DIV:
    return simpleInstruction("OP_DIV", offset);
  case OP_EQUAL:
    return simpleInstruction("OP_EQUAL", offset);
  case OP_LESS:
    return simpleInstruction("OP_LESS", offset);
  case OP_GREATER:
    return simpleInstruction("OP_GREATER", offset);
  case OP_DEFINE_GLOBAL:
    return constantInstruction("OP_DEFINE_GLOBAL", chunk, offset);
  case OP_GET_GLOBAL:
    return constantInstruction("OP_GET_GLOBAL", chunk, offset);
  case OP_SET_GLOBAL:
    return constantInstruction("OP_SET_GLOBAL", chunk, offset);
  case OP_GET_LOCAL:
    return byteInstruction("OP_GET_LOCAL", chunk, offset);
  case OP_SET_LOCAL:
    return byteInstruction("OP_SET_LOCAL", chunk, offset);
  case OP_JUMP:
    return jumpInstruction("OP_JUMP", 1, chunk, offset);
  case OP_JUMP_IF_FALSE:
    return jumpInstruction("OP_JUMP_IF_FALSE", 1, chunk, offset);
  case OP_LOOP:
    return jumpInstruction("OP_LOOP", -1, chunk, offset);
  case OP_GET_UPVALUE:
    return byteInstruction("OP_GET_UPVALUE", chunk, offset);
  case OP_SET_UPVALUE:
    return byteInstruction("OP_SET_UPVALUE", chunk, offset);
  case OP_CALL:
    return byteInstruction("OP_CALL", chunk, offset);
  case OP_INVOKE:
    return invokeInstruction("OP_INVOKE", chunk, offset);
  case OP_BUILD_ARRAY:
    return byteInstruction("OP_BUILD_ARRAY", chunk, offset);
  case OP_BUILD_MAP:
    return byteInstruction("OP_BUILD_MAP", chunk, offset);
  case OP_INDEX_GET:
    return simpleInstruction("OP_INDEX_GET", offset);
  case OP_INDEX_SET:
    return simpleInstruction("OP_INDEX_SET", offset);
  case OP_CLOSURE: {
    // Variable-length: the function constant, then 2 bytes per upvalue. We print
    // the function and one line per captured upvalue describing its source.
    offset++; // past the opcode
    uint8_t constant = chunk->code[offset++];
    printf("%-16s %4d '", "OP_CLOSURE", constant);
    printValue(chunk->constants.values[constant]);
    printf("'\n");
    ObjFunction *function = AS_FUNCTION(chunk->constants.values[constant]);
    for (int j = 0; j < function->upvalueCount; j++) {
      int isLocal = chunk->code[offset++];
      int index = chunk->code[offset++];
      printf("%04d    |                     %s %d\n", offset - 2,
             isLocal ? "local" : "upvalue", index);
    }
    return offset;
  }
  case OP_CLOSE_UPVALUE:
    return simpleInstruction("OP_CLOSE_UPVALUE", offset);
  case OP_PRINT:
    return simpleInstruction("OP_PRINT", offset);
  case OP_POP:
    return simpleInstruction("OP_POP", offset);
  case OP_RETURN:
    return simpleInstruction("OP_RETURN", offset);
  default:
    printf("Unknown opcode %d\n", instruction);
    return offset + 1;
  }
}

#include <stdio.h>
#include <stdlib.h>

#include "chunk.h"
#include "object.h" // AS_FUNCTION — CLOSURE's length depends on its upvalue count

void initChunk(Chunk *chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  initValueArray(&chunk->constants);
  chunk->globalCache = NULL; // allocated lazily on the first global read
  chunk->debugLocals = NULL;
  chunk->debugLocalCount = 0;
  chunk->debugLocalCapacity = 0;
}

void freeChunk(Chunk *chunk) {
  free(chunk->code);
  free(chunk->lines);
  free(chunk->globalCache); // plain malloc'd, not GC-managed
  free(chunk->debugLocals);
  freeValueArray(&chunk->constants);
  initChunk(chunk);
}

int chunkAddLocalDebug(Chunk *chunk, ObjString *name, int slot, int start) {
  if (chunk->debugLocalCount + 1 > chunk->debugLocalCapacity) {
    chunk->debugLocalCapacity =
        chunk->debugLocalCapacity < 8 ? 8 : chunk->debugLocalCapacity * 2;
    chunk->debugLocals = realloc(chunk->debugLocals,
                                 sizeof(LocalDebug) * chunk->debugLocalCapacity);
    if (chunk->debugLocals == NULL) {
      fprintf(stderr, "cnano: out of memory recording debug info\n");
      exit(70);
    }
  }
  LocalDebug *d = &chunk->debugLocals[chunk->debugLocalCount];
  d->name = name;
  d->slot = slot;
  d->startOffset = start;
  d->endOffset = -1; // still open; closed by the compiler at end of scope
  return chunk->debugLocalCount++;
}

void writeChunk(Chunk *chunk, uint8_t byte, int line) {
  // Same grow-by-doubling strategy as ValueArray. `code` and `lines` grow in
  // lockstep so that lines[i] always describes code[i].
  if (chunk->capacity < chunk->count + 1) {
    int oldCapacity = chunk->capacity;
    chunk->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
    chunk->code = realloc(chunk->code, sizeof(uint8_t) * chunk->capacity);
    chunk->lines = realloc(chunk->lines, sizeof(int) * chunk->capacity);
    if (chunk->code == NULL || chunk->lines == NULL) {
      fprintf(stderr, "cnano: out of memory growing chunk\n");
      exit(70);
    }
  }
  chunk->code[chunk->count] = byte;
  chunk->lines[chunk->count] = line;
  chunk->count++;
}

int addConstant(Chunk *chunk, Value value) {
  return writeValueArray(&chunk->constants, value);
}

int instructionLength(Chunk *chunk, int offset) {
  switch (chunk->code[offset]) {
  case OP_CONSTANT_LONG:
    return 4;
  case OP_JUMP:
  case OP_JUMP_IF_FALSE:
  case OP_LOOP:
  case OP_BEGIN_TRY:
  case OP_INVOKE:
    return 3;
  case OP_CONSTANT:
  case OP_DEFINE_GLOBAL:
  case OP_GET_GLOBAL:
  case OP_SET_GLOBAL:
  case OP_GET_LOCAL:
  case OP_SET_LOCAL:
  case OP_GET_UPVALUE:
  case OP_SET_UPVALUE:
  case OP_CALL:
  case OP_TAIL_CALL:
  case OP_BUILD_ARRAY:
  case OP_BUILD_MAP:
  case OP_GET_FIELD:
  case OP_SET_FIELD:
  case OP_METHOD:
  case OP_IS_KIND:
    return 2;
  case OP_CLOSURE: {
    // Variable length: the function constant, then two bytes per upvalue.
    ObjFunction *fn = AS_FUNCTION(chunk->constants.values[chunk->code[offset + 1]]);
    return 2 + 2 * fn->upvalueCount;
  }
  default:
    return 1; // every operand-less opcode
  }
}

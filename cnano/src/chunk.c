#include <stdio.h>
#include <stdlib.h>

#include "chunk.h"

void initChunk(Chunk *chunk) {
  chunk->count = 0;
  chunk->capacity = 0;
  chunk->code = NULL;
  chunk->lines = NULL;
  initValueArray(&chunk->constants);
  chunk->globalCache = NULL; // allocated lazily on the first global read
}

void freeChunk(Chunk *chunk) {
  free(chunk->code);
  free(chunk->lines);
  free(chunk->globalCache); // plain malloc'd, not GC-managed
  freeValueArray(&chunk->constants);
  initChunk(chunk);
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

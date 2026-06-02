#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "dce.h"

static bool isJump(uint8_t op) {
  return op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_LOOP ||
         op == OP_BEGIN_TRY;
}
static int jumpTarget(Chunk *c, int off) {
  int operand = (c->code[off + 1] << 8) | c->code[off + 2];
  return c->code[off] == OP_LOOP ? off + 3 - operand : off + 3 + operand;
}

// Rebuild `chunk` keeping only bytes not marked in `deleted`, recomputing every
// surviving jump's relative operand against an old->new offset map. (Same shape
// as the peephole's compaction; here the deleted ranges are whole unreachable
// blocks. We only ever delete, so the result fits in place.)
static void compact(Chunk *chunk, const bool *deleted) {
  int n = chunk->count;
  int *map = malloc(sizeof(int) * (n + 1));
  bool *isStart = calloc(n + 1, sizeof(bool));
  if (map == NULL || isStart == NULL) { free(map); free(isStart); return; }
  for (int off = 0; off < n;)
    { isStart[off] = true; off += instructionLength(chunk, off); }

  int cursor = 0;
  for (int off = 0; off <= n; off++) {
    map[off] = cursor;
    if (off < n && isStart[off] && !deleted[off])
      cursor += instructionLength(chunk, off);
  }

  uint8_t *newCode = malloc((size_t)cursor + 1);
  int *newLines = malloc(sizeof(int) * ((size_t)cursor + 1));
  if (newCode == NULL || newLines == NULL) {
    free(map); free(isStart); free(newCode); free(newLines);
    return;
  }
  int w = 0;
  for (int off = 0; off < n;) {
    int len = instructionLength(chunk, off);
    if (!deleted[off]) {
      for (int k = 0; k < len; k++) {
        newCode[w] = chunk->code[off + k];
        newLines[w] = chunk->lines[off + k];
        w++;
      }
      if (isJump(chunk->code[off])) {
        int newStart = map[off];
        int newTarget = map[jumpTarget(chunk, off)];
        int operand = chunk->code[off] == OP_LOOP ? (newStart + 3) - newTarget
                                                  : newTarget - (newStart + 3);
        newCode[newStart + 1] = (uint8_t)((operand >> 8) & 0xff);
        newCode[newStart + 2] = (uint8_t)(operand & 0xff);
      }
    }
    off += len;
  }
  memcpy(chunk->code, newCode, (size_t)w);
  memcpy(chunk->lines, newLines, sizeof(int) * (size_t)w);
  chunk->count = w;
  free(map); free(isStart); free(newCode); free(newLines);
}

static void eliminateChunk(Chunk *chunk) {
  if (chunk->count == 0)
    return;
  CFG *cfg = buildCFG(chunk);
  if (cfg == NULL)
    return;
  bool *deleted = calloc(chunk->count, sizeof(bool));
  if (deleted == NULL) { freeCFG(cfg); return; }
  bool anyDead = false;
  for (int i = 0; i < cfg->count; i++)
    if (!cfg->blocks[i].reachable) {
      for (int off = cfg->blocks[i].start; off < cfg->blocks[i].end; off++)
        deleted[off] = true;
      anyDead = true;
    }
  freeCFG(cfg);
  if (anyDead)
    compact(chunk, deleted);
  free(deleted);
}

void eliminateDeadBlocks(ObjFunction *fn) {
  eliminateChunk(&fn->chunk);
  for (int i = 0; i < fn->chunk.constants.count; i++)
    if (IS_FUNCTION(fn->chunk.constants.values[i]))
      eliminateDeadBlocks(AS_FUNCTION(fn->chunk.constants.values[i]));
}

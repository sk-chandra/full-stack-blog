#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "chunk.h"
#include "peephole.h"

// The byte length of the instruction at `off` (shared with the CFG builder).
static int instrLen(Chunk *c, int off) { return instructionLength(c, off); }

static bool isJump(uint8_t op) {
  return op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_LOOP ||
         op == OP_BEGIN_TRY;
}

// Pushes whose value has NO side effect and cannot error — so producing one and
// immediately popping it is a true no-op we can delete. (GET_GLOBAL is excluded:
// reading an undefined global is a deliberate runtime error.)
static bool isPurePush(uint8_t op) {
  return op == OP_CONSTANT || op == OP_CONSTANT_LONG || op == OP_NIL ||
         op == OP_TRUE || op == OP_FALSE || op == OP_GET_LOCAL ||
         op == OP_GET_UPVALUE;
}

// The absolute target offset of the jump at `off` (forward for JUMP/JUMP_IF_FALSE/
// BEGIN_TRY, backward for LOOP). Offsets are relative to the instruction AFTER the
// 3-byte jump.
static int jumpTarget(Chunk *c, int off) {
  int operand = (c->code[off + 1] << 8) | c->code[off + 2];
  return c->code[off] == OP_LOOP ? off + 3 - operand : off + 3 + operand;
}

void peepholeOptimize(Chunk *chunk);

void peepholeFunction(ObjFunction *fn) {
  peepholeOptimize(&fn->chunk);
  // Recurse into nested functions, which ride in this chunk's constant pool.
  for (int i = 0; i < fn->chunk.constants.count; i++)
    if (IS_FUNCTION(fn->chunk.constants.values[i]))
      peepholeFunction(AS_FUNCTION(fn->chunk.constants.values[i]));
}

void peepholeOptimize(Chunk *chunk) {
  int n = chunk->count;
  if (n == 0)
    return;

  // Mark which byte offsets begin an instruction, and which are jump targets
  // (we must never delete a target — a jump would then land mid-instruction).
  bool *isStart = calloc(n + 1, sizeof(bool));
  bool *isTarget = calloc(n + 1, sizeof(bool));
  bool *deleted = calloc(n + 1, sizeof(bool));
  if (!isStart || !isTarget || !deleted) {
    free(isStart); free(isTarget); free(deleted);
    return; // out of memory: skip the optional optimisation
  }
  for (int off = 0; off < n;) {
    isStart[off] = true;
    if (isJump(chunk->code[off]))
      isTarget[jumpTarget(chunk, off)] = true;
    off += instrLen(chunk, off);
  }

  // Pattern pass: mark deletions. Only delete instructions that are NOT jump
  // targets, so every surviving jump still lands on a real instruction boundary.
  for (int off = 0; off < n;) {
    int len = instrLen(chunk, off);
    int next = off + len;
    uint8_t op = chunk->code[off];
    if (next < n && isStart[next] && !isTarget[off] && !isTarget[next]) {
      uint8_t op2 = chunk->code[next];
      bool pair = (isPurePush(op) && op2 == OP_POP) ||      // push then discard
                  (op == OP_NOT && op2 == OP_NOT) ||         // !!x
                  (op == OP_NEGATE && op2 == OP_NEGATE);     // - -x
      if (pair) {
        deleted[off] = true;
        deleted[next] = true;
        off = next + instrLen(chunk, next); // don't re-match the deleted second op
        continue;
      }
    }
    off = next;
  }

  // Build the old->new offset map. A deleted instruction maps to where the next
  // survivor lands (harmless, since targets are never deleted). `map[n]` is the
  // new end, so a jump to end-of-chunk remaps correctly.
  int *map = malloc(sizeof(int) * (n + 1));
  if (!map) { free(isStart); free(isTarget); free(deleted); return; }
  int cursor = 0;
  for (int off = 0; off <= n; off++) {
    if (off == n || isStart[off])
      map[off] = cursor;
    else
      map[off] = -1; // interior byte; never used as a jump target
    if (off < n && isStart[off] && !deleted[off])
      cursor += instrLen(chunk, off);
  }

  // Emit the survivors into temp buffers, rewriting jump operands against the
  // map. Peephole only ever deletes, so the result fits in the existing arrays.
  uint8_t *newCode = malloc((size_t)cursor + 1);
  int *newLines = malloc(sizeof(int) * ((size_t)cursor + 1));
  if (!newCode || !newLines) {
    free(isStart); free(isTarget); free(deleted); free(map);
    free(newCode); free(newLines);
    return;
  }
  int w = 0;
  for (int off = 0; off < n;) {
    int len = instrLen(chunk, off);
    if (!deleted[off]) {
      for (int k = 0; k < len; k++) {
        newCode[w] = chunk->code[off + k];
        newLines[w] = chunk->lines[off + k];
        w++;
      }
      if (isJump(chunk->code[off])) {
        // Recompute this jump's 2-byte operand from the remapped target.
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

  // Copy back in place and shrink the count (capacity is left untouched).
  memcpy(chunk->code, newCode, (size_t)w);
  memcpy(chunk->lines, newLines, sizeof(int) * (size_t)w);
  chunk->count = w;

  free(isStart);
  free(isTarget);
  free(deleted);
  free(map);
  free(newCode);
  free(newLines);
}

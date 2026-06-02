#include <stdio.h>
#include <stdlib.h>

#include "cfg.h"
#include "debug.h" // disassembleInstruction — reused to print a block's code

// The absolute target of a jump at `off` (forward for JUMP/JUMP_IF_FALSE/
// BEGIN_TRY, backward for LOOP). Offsets are relative to the byte after the jump.
static int jumpTarget(Chunk *c, int off) {
  int operand = (c->code[off + 1] << 8) | c->code[off + 2];
  return c->code[off] == OP_LOOP ? off + 3 - operand : off + 3 + operand;
}

static bool isJump(uint8_t op) {
  return op == OP_JUMP || op == OP_JUMP_IF_FALSE || op == OP_LOOP ||
         op == OP_BEGIN_TRY;
}

// The block index whose `start` equals `off` (jump targets always land on a
// leader, so this never fails for a real edge), or -1.
static int blockAt(CFG *cfg, int off) {
  for (int i = 0; i < cfg->count; i++)
    if (cfg->blocks[i].start == off)
      return i;
  return -1;
}

// The byte offset of the LAST instruction in [start, end) — what determines the
// block's outgoing edges.
static int lastInstr(Chunk *chunk, int start, int end) {
  int off = start, last = start;
  while (off < end) {
    last = off;
    off += instructionLength(chunk, off);
  }
  return last;
}

CFG *buildCFG(Chunk *chunk) {
  int n = chunk->count;
  CFG *cfg = malloc(sizeof(CFG));
  if (cfg == NULL)
    return NULL;
  cfg->chunk = chunk;
  if (n == 0) { // an empty chunk: one reachable block that falls straight off end
    cfg->blocks = malloc(sizeof(BasicBlock));
    cfg->blocks[0] = (BasicBlock){0, 0, {-1, -1}, 0, true, true};
    cfg->count = 1;
    return cfg;
  }

  // Pass 1: mark LEADERS — the entry, every jump target, and every instruction
  // right after a control transfer (so the transfer ends a block).
  bool *leader = calloc(n, sizeof(bool));
  if (leader == NULL) { free(cfg); return NULL; }
  leader[0] = true;
  for (int off = 0; off < n;) {
    uint8_t op = chunk->code[off];
    int next = off + instructionLength(chunk, off);
    if (isJump(op)) {
      leader[jumpTarget(chunk, off)] = true;
      if (next < n)
        leader[next] = true;
    } else if ((op == OP_RETURN || op == OP_THROW) && next < n) {
      leader[next] = true;
    }
    off = next;
  }

  // Pass 2: leaders (in order) delimit the blocks.
  int count = 0;
  for (int off = 0; off < n; off++)
    if (leader[off])
      count++;
  cfg->blocks = malloc(sizeof(BasicBlock) * count);
  cfg->count = count;
  int b = 0;
  for (int off = 0; off < n;) {
    if (!leader[off]) { off += instructionLength(chunk, off); continue; }
    int end = off + instructionLength(chunk, off);
    while (end < n && !leader[end])
      end += instructionLength(chunk, end);
    cfg->blocks[b] = (BasicBlock){off, end, {-1, -1}, 0, false, false};
    b++;
    off = end;
  }

  // Pass 3: edges. Compute each block's successor OFFSETS from its last
  // instruction, then map them to block indices — except an offset of `n` (past
  // the last instruction) is not a block: it means control falls off the end.
  for (int i = 0; i < cfg->count; i++) {
    BasicBlock *blk = &cfg->blocks[i];
    int li = lastInstr(chunk, blk->start, blk->end);
    uint8_t op = chunk->code[li];
    int soff[CFG_MAX_SUCC];
    int sn = 0;
    if (op == OP_RETURN || op == OP_THROW) {
      sn = 0; // exits the function
    } else if (op == OP_JUMP || op == OP_LOOP) {
      soff[sn++] = jumpTarget(chunk, li);
    } else if (op == OP_JUMP_IF_FALSE || op == OP_BEGIN_TRY) {
      soff[sn++] = blk->end;             // fall-through
      soff[sn++] = jumpTarget(chunk, li); // taken
    } else {
      soff[sn++] = blk->end; // straight-line fall-through
    }
    for (int s = 0; s < sn; s++) {
      if (soff[s] >= n)
        blk->fallsOffEnd = true; // would run past the last instruction
      else
        blk->succ[blk->succCount++] = blockAt(cfg, soff[s]);
    }
  }

  // Pass 4: mark reachable blocks by a worklist walk from the entry.
  int *stack = malloc(sizeof(int) * cfg->count);
  int sp = 0;
  cfg->blocks[0].reachable = true;
  stack[sp++] = 0;
  while (sp > 0) {
    BasicBlock *blk = &cfg->blocks[stack[--sp]];
    for (int s = 0; s < blk->succCount; s++) {
      int t = blk->succ[s];
      if (t >= 0 && !cfg->blocks[t].reachable) {
        cfg->blocks[t].reachable = true;
        stack[sp++] = t;
      }
    }
  }
  free(stack);
  free(leader);
  return cfg;
}

bool cfgReachesEnd(CFG *cfg) {
  for (int i = 0; i < cfg->count; i++)
    if (cfg->blocks[i].reachable && cfg->blocks[i].fallsOffEnd)
      return true;
  return false;
}

void freeCFG(CFG *cfg) {
  if (cfg == NULL)
    return;
  free(cfg->blocks);
  free(cfg);
}

void printCFG(CFG *cfg, const char *name) {
  printf("== CFG: %s (%d block%s) ==\n", name, cfg->count,
         cfg->count == 1 ? "" : "s");
  for (int i = 0; i < cfg->count; i++) {
    BasicBlock *blk = &cfg->blocks[i];
    printf("B%d [%04d..%04d]%s", i, blk->start, blk->end,
           blk->reachable ? "" : "  (unreachable)");
    printf("  ->");
    for (int s = 0; s < blk->succCount; s++)
      printf(" B%d", blk->succ[s]);
    if (blk->fallsOffEnd)
      printf(" (end)");
    if (blk->succCount == 0 && !blk->fallsOffEnd)
      printf(" (exit)");
    printf("\n");
    for (int off = blk->start; off < blk->end;)
      off = disassembleInstruction(cfg->chunk, off);
  }
  printf("\n");
}

// cfg.h — a control-flow graph over a function's bytecode.
//
// A CFG is the backbone of every serious compiler analysis. We split a chunk's
// flat instruction stream into BASIC BLOCKS — maximal runs of straight-line code
// with a single entry and single exit — and connect them with edges that mirror
// the jumps. Once you have this, data-flow analyses (reachability, liveness,
// constant propagation, …) are just iterating to a fixed point over the graph.
#ifndef CNANO_CFG_H
#define CNANO_CFG_H

#include "chunk.h"

#define CFG_MAX_SUCC 2 // a block ends in at most a 2-way branch (cond jump / try)

typedef struct {
  int start;            // byte offset of the block's first instruction
  int end;              // byte offset just past its last instruction
  int succ[CFG_MAX_SUCC];
  int succCount;        // 0 (a return/throw block), 1 (jump/fallthrough), or 2
  bool reachable;       // filled by a forward walk from the entry block
  bool fallsOffEnd;     // control can run PAST the last instruction (no return)
} BasicBlock;

typedef struct {
  BasicBlock *blocks;
  int count;
  Chunk *chunk;         // the bytecode this CFG describes (borrowed)
} CFG;

// Build the CFG for `chunk` (caller owns it; free with freeCFG). The entry is
// always block 0. Marks each block's `reachable` flag from the entry.
CFG *buildCFG(Chunk *chunk);
void freeCFG(CFG *cfg);

// True if control can reach the END of the chunk — i.e. some REACHABLE block
// runs off the last instruction without returning. Used to detect a function
// that can fall through to its implicit `return nil` (all-paths-return check).
bool cfgReachesEnd(CFG *cfg);

// Print the CFG (blocks, their disassembled instructions, and successor edges)
// to stdout — the `--cfg` learning aid, the data-flow analogue of `--dump`.
void printCFG(CFG *cfg, const char *name);

#endif // CNANO_CFG_H

// dce.h — dead-block elimination: drop unreachable basic blocks.
//
// The simplest, safest CFG-driven optimization. The reachability walk in cfg.c
// already finds blocks with no path from the entry — code the VM can never run
// (compiler artifacts like the jump-over an if-branch that returns, or the
// implicit trailing return after an explicit one). Removing them can't change
// behaviour; the only care needed is recomputing the jump offsets that span the
// removed bytes (exactly the peephole's concern, here driven by the CFG).
#ifndef CNANO_DCE_H
#define CNANO_DCE_H

#include "object.h"

// Remove unreachable blocks from `fn`'s chunk and, recursively, every nested
// function. Runs on a successfully compiled program only.
void eliminateDeadBlocks(ObjFunction *fn);

#endif // CNANO_DCE_H

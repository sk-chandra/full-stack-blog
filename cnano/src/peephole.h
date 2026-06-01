// peephole.h — a tiny bytecode peephole optimiser (a post-compilation pass).
//
// "Peephole" optimisation slides a small window over the finished bytecode and
// rewrites locally-redundant instruction sequences. The interesting part — and
// the reason this is its own file — is that deleting bytes shifts every later
// instruction, so all JUMP offsets that span the hole must be recomputed. That
// jump-target remapping is the lesson here; the speedup on already-constant-
// folded code is deliberately modest.
#ifndef CNANO_PEEPHOLE_H
#define CNANO_PEEPHOLE_H

#include "object.h"

// Optimise `fn`'s chunk and, recursively, every nested function it defines.
// Runs only on a successfully compiled program (a malformed chunk is left alone).
void peepholeFunction(ObjFunction *fn);

#endif // CNANO_PEEPHOLE_H

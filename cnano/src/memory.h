// memory.h — the central memory manager and garbage collector.
//
// Until now cnano allocated objects with malloc and freed them all at once at
// shutdown (freeObjects). That is fine for a program that runs briefly, but a
// loop that builds a million temporary strings would grow without bound: nothing
// reclaims the dead ones WHILE the program runs. This file fixes that with a
// real, tracing GARBAGE COLLECTOR.
//
// The algorithm is MARK-AND-SWEEP, the classic tracing collector:
//   1. MARK   — starting from the "roots" (values the program can still reach:
//               the stack, call frames, globals, open upvalues), follow every
//               reference and mark each reachable object as live.
//   2. SWEEP  — walk the list of all objects and free every one that was NOT
//               marked: if nothing reachable points at it, the program can never
//               use it again, so its memory is safe to reclaim.
// We use the tri-colour abstraction (white = unprocessed, grey = found but its
// own references not yet scanned, black = fully scanned) with an explicit grey
// WORKLIST, so tracing is an iterative loop rather than deep recursion.
//
// A DESIGN DECISION specific to cnano: the collector runs only DURING EXECUTION,
// never during parsing/compilation. cnano (unlike a single-pass compiler) builds
// a separate AST that holds ObjString* pointers which are not reachable from any
// GC root — collecting then would free strings the AST still needs. Since
// compilation finishes before the VM starts, we simply keep the collector
// quiescent until execution begins (see vm.gcEnabled). This sidesteps the
// "compiler roots" problem cleanly instead of papering over it.
#ifndef CNANO_MEMORY_H
#define CNANO_MEMORY_H

#include "common.h"
#include "value.h"

// The single allocation choke point. Grows, shrinks, allocates (oldSize 0) or
// frees (newSize 0) a block, keeping a running total of live bytes and triggering
// a collection when that total crosses a threshold. Routing allocation through
// one function is what lets the GC know how much memory is in play.
void *reallocate(void *pointer, size_t oldSize, size_t newSize);

// Mark a single object / value as reachable (the root-marking and tracing steps
// call these). Safe to call with NULL or a non-object value.
void markObject(Obj *object);
void markValue(Value value);

// Run one full mark-and-sweep collection now. Normally invoked automatically by
// reallocate; exposed so a stress-test build can force it on every allocation.
void collectGarbage(void);

#endif // CNANO_MEMORY_H

#include <stdio.h>
#include <stdlib.h>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "vm.h"

// When a collection finishes, the next one is scheduled for when live memory has
// grown by this factor. A self-tuning threshold like this keeps GC overhead
// roughly proportional to the live set: programs that hold little are collected
// rarely, programs that hold lots are collected more often.
#define GC_HEAP_GROW_FACTOR 2

// The floor for the next-collection threshold, so tiny programs don't collect on
// every other allocation. Kept modest so the test suite actually exercises the
// collector rather than never reaching it.
#define GC_MIN_NEXT (64 * 1024)

void *reallocate(void *pointer, size_t oldSize, size_t newSize) {
  // Keep the running tally first, so the trigger below sees up-to-date numbers.
  vm.bytesAllocated += newSize - oldSize;
  if (vm.collectStats && newSize > oldSize) { // count growth as allocation
    vm.allocBytes += newSize - oldSize;
    if (oldSize == 0)
      vm.allocCount++; // a brand-new block (vs. growing an existing one)
  }

  // Only consider collecting when we are GROWING the heap, and only once the VM
  // is actually executing (see the design note in memory.h). Shrinking/freeing
  // can never make us run out of memory, so it never triggers a collection.
  if (newSize > oldSize && vm.gcEnabled) {
#ifdef DEBUG_STRESS_GC
    collectGarbage(); // stress mode: collect on EVERY growth, to surface bugs
#else
    if (vm.bytesAllocated > vm.nextGC)
      collectGarbage();
#endif
  }

  if (newSize == 0) {
    free(pointer);
    return NULL;
  }
  void *result = realloc(pointer, newSize);
  if (result == NULL) {
    fprintf(stderr, "cnano: out of memory\n");
    exit(70);
  }
  return result;
}

// --- the MARK phase --------------------------------------------------------

void markObject(Obj *object) {
  if (object == NULL)
    return;
  if (object->isMarked)
    return; // already grey/black — avoids cycles looping forever
  object->isMarked = true;

  // Push onto the grey worklist (objects found, but whose own references we have
  // not scanned yet). The grey stack is plain malloc memory the GC manages by
  // hand — it must NOT go through reallocate, or marking could trigger a nested
  // collection. If we can't grow it, we genuinely cannot collect, so we abort.
  if (vm.grayCapacity < vm.grayCount + 1) {
    vm.grayCapacity = vm.grayCapacity < 8 ? 8 : vm.grayCapacity * 2;
    vm.grayStack =
        (Obj **)realloc(vm.grayStack, sizeof(Obj *) * vm.grayCapacity);
    if (vm.grayStack == NULL) {
      fprintf(stderr, "cnano: out of memory (GC grey stack)\n");
      exit(70);
    }
  }
  vm.grayStack[vm.grayCount++] = object;
}

void markValue(Value value) {
  // Only heap objects participate in GC; ints/bools/nil live inline in the Value.
  if (IS_OBJ(value))
    markObject(AS_OBJ(value));
}

static void markArray(ValueArray *array) {
  for (int i = 0; i < array->count; i++)
    markValue(array->values[i]);
}

// "Blacken" an object: it was grey (reachable), now scan ITS references and mark
// them, turning it black. The work depends on the object's type — this is the one
// place that must know what each object can point at.
static void blackenObject(Obj *object) {
  switch (object->type) {
  case OBJ_STRING:
  case OBJ_NATIVE:
    break; // these reference no other heap objects
  case OBJ_ARRAY:
    // An array keeps every element reachable.
    markArray(&((ObjArray *)object)->elements);
    break;
  case OBJ_MAP: {
    // A map keeps every live key AND value reachable.
    ObjMap *map = (ObjMap *)object;
    for (int i = 0; i < map->capacity; i++) {
      if (map->entries[i].state == MAP_OCCUPIED) {
        markValue(map->entries[i].key);
        markValue(map->entries[i].value);
      }
    }
    break;
  }
  case OBJ_STRUCT: {
    // Keep the struct's name, every field name, and every method alive.
    ObjStruct *s = (ObjStruct *)object;
    markObject((Obj *)s->name);
    for (int i = 0; i < s->fieldCount; i++)
      markObject((Obj *)s->fieldNames[i]);
    markTable(&s->methods);
    break;
  }
  case OBJ_ENUM: {
    // Keep the enum's name and every member alive (the table marks names+values).
    ObjEnum *e = (ObjEnum *)object;
    markObject((Obj *)e->name);
    markTable(&e->members);
    break;
  }
  case OBJ_ENUM_MEMBER: {
    // Keep the parent enum and this member's own name alive.
    ObjEnumMember *m = (ObjEnumMember *)object;
    markObject((Obj *)m->parent);
    markObject((Obj *)m->name);
    break;
  }
  case OBJ_INSTANCE: {
    // Keep the struct type and every field (the table marks names + values).
    ObjInstance *inst = (ObjInstance *)object;
    markObject((Obj *)inst->type);
    markTable(&inst->fields);
    break;
  }
  case OBJ_UPVALUE:
    // A closed upvalue owns a heap value; keep whatever it holds alive.
    markValue(((ObjUpvalue *)object)->closed);
    break;
  case OBJ_FUNCTION: {
    // A function keeps its name and every constant in its chunk reachable
    // (string/function constants live there).
    ObjFunction *function = (ObjFunction *)object;
    markObject((Obj *)function->name);
    markArray(&function->chunk.constants);
    break;
  }
  case OBJ_CLOSURE: {
    // A closure keeps its function and every captured upvalue alive.
    ObjClosure *closure = (ObjClosure *)object;
    markObject((Obj *)closure->function);
    for (int i = 0; i < closure->upvalueCount; i++)
      markObject((Obj *)closure->upvalues[i]);
    break;
  }
  case OBJ_GENERATOR: {
    // A suspended generator keeps its closure and every value frozen in its
    // saved stack window alive (those are unreachable any other way).
    ObjGenerator *gen = (ObjGenerator *)object;
    markObject((Obj *)gen->closure);
    for (int i = 0; i < gen->savedCount; i++)
      markValue(gen->saved[i]);
    break;
  }
  }
}

// Mark the ROOTS: every object the running program can reach directly, without
// going through another object. Get this set wrong (miss a root) and the GC frees
// something still in use — the single most important function to get right.
static void markRoots(void) {
  // 1. Everything currently on the operand stack.
  for (Value *slot = vm.stack; slot < vm.stackTop; slot++)
    markValue(*slot);

  // 2. The closure running in each active call frame.
  for (int i = 0; i < vm.frameCount; i++)
    markObject((Obj *)vm.frames[i].closure);

  // 3. Every open upvalue (it points at a live stack slot, but the upvalue OBJECT
  //    itself must survive so closures keep sharing it).
  for (ObjUpvalue *upvalue = vm.openUpvalues; upvalue != NULL;
       upvalue = upvalue->next)
    markObject((Obj *)upvalue);

  // 4. The global-variable table (both the name keys and the values).
  markTable(&vm.globals);

  // 5. The interned "init" name (used to find constructors).
  markObject((Obj *)vm.initString);

  // NOTE: vm.strings (the string intern pool) is deliberately NOT a root. It is a
  // WEAK table — it must not, by itself, keep a string alive. We prune its dead
  // entries in collectGarbage() after tracing, just before sweeping.
}

// Drain the grey worklist: repeatedly take a grey object and blacken it. Because
// blackening can mark new objects (pushing them grey), this loop continues until
// the worklist is empty — at which point every reachable object is black.
static void traceReferences(void) {
  while (vm.grayCount > 0) {
    Obj *object = vm.grayStack[--vm.grayCount];
    blackenObject(object);
  }
}

// --- the SWEEP phase -------------------------------------------------------

// Walk the intrusive list of all objects. Survivors (marked) are unmarked for the
// next cycle; the rest (white) are unreachable and freed, unlinked in place.
static void sweep(void) {
  Obj *previous = NULL;
  Obj *object = vm.objects;
  while (object != NULL) {
    if (object->isMarked) {
      object->isMarked = false; // reset to white for the next collection
      previous = object;
      object = object->next;
    } else {
      Obj *unreached = object;
      object = object->next;
      if (previous != NULL)
        previous->next = object;
      else
        vm.objects = object;
      freeObject(unreached);
    }
  }
}

void collectGarbage(void) {
  if (vm.collectStats)
    vm.gcCount++;
#ifdef DEBUG_LOG_GC
  printf("-- gc begin\n");
  size_t before = vm.bytesAllocated;
#endif

  markRoots();
  traceReferences();
  // Weak references: any interned string NOT marked above is about to be swept,
  // so remove it from the intern pool first to avoid a dangling table entry.
  tableRemoveWhite(&vm.strings);
  sweep();

  // Schedule the next collection relative to what survived.
  vm.nextGC = vm.bytesAllocated * GC_HEAP_GROW_FACTOR;
  if (vm.nextGC < GC_MIN_NEXT)
    vm.nextGC = GC_MIN_NEXT;

#ifdef DEBUG_LOG_GC
  printf("-- gc end: collected %zu bytes (now %zu), next at %zu\n",
         before - vm.bytesAllocated, vm.bytesAllocated, vm.nextGC);
#endif
}

// tclass.c — the shared IR type-class analysis. See tclass.h for the idea.
#include <stdlib.h>
#include <string.h>

#include "object.h" // ObjString->chars
#include "tclass.h"

bool tcIsComparison(NodeOp op) {
  return op == OP_NODE_EQUAL || op == OP_NODE_LESS || op == OP_NODE_GREATER;
}
bool tcIsArith(NodeOp op) {
  return op == OP_NODE_ADD || op == OP_NODE_SUB || op == OP_NODE_MUL ||
         op == OP_NODE_DIV;
}

int findFuncIndex(IRModule *m, ObjString *name) {
  for (int k = 0; k < m->count; k++)
    if (strcmp(m->funcs[k]->name, name->chars) == 0)
      return k;
  return -1;
}

int varIndexIn(ObjString *name, FnInfo *info) {
  for (int v = 0; v < info->varCount; v++)
    if (info->vars[v] == name)
      return v;
  return -1;
}

ObjString **collectVars(IRFunc *fn, int *outCount) {
  ObjString **vars = NULL;
  int count = 0, cap = 0;
  for (int p = 0; p < fn->paramCount; p++) {
    if (count + 1 > cap) { cap = cap < 8 ? 8 : cap * 2; vars = realloc(vars, sizeof(ObjString *) * cap); }
    vars[count++] = fn->params[p];
  }
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead || (in->op != IR_LOAD && in->op != IR_STORE))
      continue;
    bool seen = false;
    for (int v = 0; v < count; v++)
      if (vars[v] == in->var) { seen = true; break; }
    if (!seen) {
      if (count + 1 > cap) { cap = cap < 8 ? 8 : cap * 2; vars = realloc(vars, sizeof(ObjString *) * cap); }
      vars[count++] = in->var;
    }
  }
  *outCount = count;
  return vars;
}

// Join `c` into `*slot`; returns true if the slot changed.
static bool joinClass(TClass *slot, TClass c) {
  if (c == TC_UNKNOWN || *slot == c || *slot == TC_CONFLICT)
    return false;
  *slot = (*slot == TC_UNKNOWN) ? c : TC_CONFLICT;
  return true;
}

// One classification sweep over function `fi`, flowing classes forward (and
// across the module: call arguments into the callee's parameters, callee
// returns into call results). Returns true if anything changed.
static bool classifySweep(IRModule *m, int fi, FnInfo *infos) {
  IRFunc *fn = m->funcs[fi];
  FnInfo *info = &infos[fi];
  bool changed = false;
  for (int i = 0; i < fn->count; i++) {
    IRInstr *in = &fn->code[i];
    if (in->dead)
      continue;
    TClass out = TC_UNKNOWN;
    switch (in->op) {
    case IR_CONST:
      out = IS_BOOL(in->constant) ? TC_BOOL
            : IS_FLOAT(in->constant) ? TC_FLOAT
            : IS_INT(in->constant) ? TC_INT
                                   : TC_CONFLICT; // nil: unrepresentable
      break;
    case IR_LOAD: {
      int v = varIndexIn(in->var, info);
      if (v >= 0)
        out = info->varClass[v];
      break;
    }
    case IR_STORE: {
      int v = varIndexIn(in->var, info);
      if (v >= 0)
        changed |= joinClass(&info->varClass[v], info->tempClass[in->a]);
      continue;
    }
    case IR_UNARY:
      out = in->nodeOp == OP_NODE_NOT      ? TC_BOOL
            : in->nodeOp == OP_NODE_BITNOT ? TC_INT
                                           : info->tempClass[in->a]; // negate
      break;
    case IR_BINARY:
      if (tcIsComparison(in->nodeOp))
        out = TC_BOOL;
      else if (tcIsArith(in->nodeOp)) {
        TClass a = info->tempClass[in->a], b = info->tempClass[in->b];
        // Mixed int/float arithmetic promotes to float, mirroring the VM.
        out = (a == TC_FLOAT || b == TC_FLOAT) ? TC_FLOAT
              : (a == TC_INT && b == TC_INT)   ? TC_INT
                                               : TC_UNKNOWN; // wait for operands
      } else
        out = TC_INT; // %, bitwise, shifts: integer-only (backends check)
      break;
    case IR_CALL: {
      int ci = findFuncIndex(m, in->var);
      if (ci >= 0) {
        out = infos[ci].retClass;
        // Flow each argument's class into the callee's parameter variable.
        IRFunc *callee = m->funcs[ci];
        for (int k = 0; k < in->callArgCount && k < callee->paramCount; k++) {
          int pv = varIndexIn(callee->params[k], &infos[ci]);
          if (pv >= 0)
            changed |= joinClass(&infos[ci].varClass[pv],
                                 info->tempClass[in->callArgs[k]]);
        }
      }
      break;
    }
    case IR_RETURN:
      if (in->a >= 0)
        changed |= joinClass(&info->retClass, info->tempClass[in->a]);
      continue;
    default:
      continue; // print/label/jump produce no value
    }
    if (in->dest >= 0)
      changed |= joinClass(&info->tempClass[in->dest], out);
  }
  return changed;
}

FnInfo *moduleClassify(IRModule *m) {
  FnInfo *infos = calloc(m->count, sizeof(FnInfo));
  for (int f = 0; f < m->count; f++) {
    infos[f].vars = collectVars(m->funcs[f], &infos[f].varCount);
    infos[f].varClass =
        calloc(infos[f].varCount > 0 ? infos[f].varCount : 1, sizeof(TClass));
    int nt = m->funcs[f]->nextTemp;
    infos[f].tempClass = calloc(nt > 0 ? nt : 1, sizeof(TClass));
    infos[f].retClass = TC_UNKNOWN;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (int f = 0; f < m->count; f++)
      changed |= classifySweep(m, f, infos);
  }
  return infos;
}

void freeModuleClasses(IRModule *m, FnInfo *infos) {
  for (int f = 0; f < m->count; f++) {
    free(infos[f].vars);
    free(infos[f].varClass);
    free(infos[f].tempClass);
  }
  (void)m;
  free(infos);
}

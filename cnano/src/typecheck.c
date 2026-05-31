#include <stdio.h>
#include <stdlib.h>

#include "typecheck.h"

// Type CONSTRUCTION lives in type.c now: primitives are shared singletons
// (typeInt(), ...) and the parametric types (array/map/function) are arena-
// allocated and freed together by freeTypes() once every type-consuming pass is
// done. The checker just calls those constructors — it no longer owns types.

// --- compatibility: the gradual-typing rule --------------------------------
//
// THE core decision of a gradual type system. `any` is compatible with
// everything in BOTH directions — that is the escape hatch that lets typed and
// untyped code mix freely. Two known types are compatible only if they match;
// the PARAMETRIC types match STRUCTURALLY — `[int]` is compatible with `[int]`
// (and with `[any]`), but not with `[bool]`. Because unannotated things are
// `any`, an error only ever fires when BOTH sides are known and genuinely
// disagree — and that rule recurses naturally into element/key/value types.
static bool compatible(Type *a, Type *b) {
  if (a->kind == TY_ANY || b->kind == TY_ANY)
    return true;
  // Nullable target `a = T?` accepts nil, a plain T (widening), or another U?.
  // (compatible(a, b) reads "a value of type b fits where a is expected".)
  if (a->kind == TY_NULLABLE) {
    if (b->kind == TY_NIL)
      return true;
    if (b->kind == TY_NULLABLE)
      return compatible(a->element, b->element);
    return compatible(a->element, b);
  }
  // A non-nullable target cannot accept a maybe-nil source (narrow it first).
  if (b->kind == TY_NULLABLE)
    return false;
  if (a->kind != b->kind)
    return false;
  switch (a->kind) {
  case TY_ARRAY:
    return compatible(a->element, b->element);
  case TY_MAP:
    return compatible(a->map.key, b->map.key) &&
           compatible(a->map.value, b->map.value);
  case TY_STRUCT:
    // Nominal typing: two struct types match iff they are the SAME named type.
    // Names are interned, so a pointer comparison suffices.
    return a->strct.name == b->strct.name;
  case TY_FUNCTION:
    if (a->fn.paramCount != b->fn.paramCount)
      return false;
    for (int i = 0; i < a->fn.paramCount; i++)
      if (!compatible(a->fn.params[i], b->fn.params[i]))
        return false;
    return compatible(a->fn.returnType, b->fn.returnType);
  default:
    return true; // same primitive kind
  }
}

// --- the checker state -----------------------------------------------------

// A lexically-scoped symbol table mapping a variable name (interned ObjString,
// compared by pointer) to its static Type. Same flat-array-with-depth design as
// the compiler's locals — globals live at depth 0, blocks/functions nest above.
typedef struct {
  ObjString *name;
  Type *type;
  int depth;
} Symbol;

#define MAX_SYMBOLS 1024

typedef struct {
  Symbol symbols[MAX_SYMBOLS];
  int symbolCount;
  int scopeDepth;
  // The declared return type of the function currently being checked, so a
  // `return EXPR;` can be validated against it. NULL at top level.
  Type *currentReturnType;
  bool hadError;
} Checker;

static Checker checker;

static void typeError(int line, const char *message) {
  fprintf(stderr, "[line %d] Type error: %s\n", line, message);
  checker.hadError = true;
}

static void beginScope(void) { checker.scopeDepth++; }

static void endScope(void) {
  checker.scopeDepth--;
  while (checker.symbolCount > 0 &&
         checker.symbols[checker.symbolCount - 1].depth > checker.scopeDepth)
    checker.symbolCount--;
}

// Bind a name to a type in the current scope.
static void declareSymbol(ObjString *name, Type *type) {
  if (checker.symbolCount == MAX_SYMBOLS)
    return; // silently ignore overflow; the compiler enforces real limits
  Symbol *s = &checker.symbols[checker.symbolCount++];
  s->name = name;
  s->type = type;
  s->depth = checker.scopeDepth;
}

// Look up a name, innermost scope first (so shadowing resolves correctly). An
// unknown name is `any`: it may be a global defined elsewhere, and the VM will
// catch a genuinely-undefined one at runtime. Staying gradual, we don't flag it.
static Type *lookupSymbol(ObjString *name) {
  for (int i = checker.symbolCount - 1; i >= 0; i--)
    if (checker.symbols[i].name == name)
      return checker.symbols[i].type;
  return typeAny();
}

// --- the struct registry ---------------------------------------------------
//
// Struct declarations create NAMED types. A `: Name` annotation parses to an
// unresolved reference (just the name); the checker matches it here, against the
// structs registered in a pre-pass, so forward references work.

#define MAX_STRUCTS 256
static struct {
  ObjString *name;
  Type *type; // a fully-resolved TY_STRUCT
} structRegistry[MAX_STRUCTS];
static int structCount;

static void registerStruct(ObjString *name, Type *type) {
  if (structCount < MAX_STRUCTS) {
    structRegistry[structCount].name = name;
    structRegistry[structCount].type = type;
    structCount++;
  }
}

// Replace an unresolved struct reference with the declared struct type. Other
// types pass through unchanged. An unknown struct name is a type error.
static Type *resolve(Type *t, int line) {
  if (t->kind == TY_NULLABLE) // resolve inside `T?` (e.g. `Point?`)
    return typeNullable(resolve(t->element, line));
  if (t->kind != TY_STRUCT || t->strct.fieldCount >= 0)
    return t; // not a reference (primitive, collection, or already resolved)
  for (int i = 0; i < structCount; i++)
    if (structRegistry[i].name == t->strct.name)
      return structRegistry[i].type;
  char msg[96];
  snprintf(msg, sizeof(msg), "unknown type '%s'", t->strct.name->chars);
  typeError(line, msg);
  return typeAny(); // treat the unknown type as dynamic so checking continues
}

// --- the walk --------------------------------------------------------------

static Type *checkExpr(Node *node);
static void checkStatement(Node *node);

// Helper: an arithmetic operand must be int OR any. Returns false (and reports)
// only when the operand is a KNOWN non-int — the gradual rule in action.
static bool requireInt(Type *t, int line, const char *what) {
  if (t->kind == TY_INT || t->kind == TY_ANY)
    return true;
  char msg[128];
  snprintf(msg, sizeof(msg), "%s must be int, got %s", what, typeName(t));
  typeError(line, msg);
  return false;
}

static Type *checkBinary(Node *node) {
  Type *l = checkExpr(node->as.binary.left);
  Type *r = checkExpr(node->as.binary.right);
  switch (node->as.binary.op) {
  case OP_NODE_ADD:
    // `+` is overloaded (int add OR string concat). If either side is known to
    // be a string, treat it as concatenation requiring both string-or-any;
    // otherwise require int-or-any. This mirrors the VM's runtime dispatch.
    if (l->kind == TY_STR || r->kind == TY_STR) {
      if (!compatible(l, typeStr()) || !compatible(r, typeStr()))
        typeError(node->line, "both operands of '+' must be str for concatenation");
      return (l->kind == TY_ANY || r->kind == TY_ANY) ? typeAny() : typeStr();
    }
    requireInt(l, node->line, "left operand of '+'");
    requireInt(r, node->line, "right operand of '+'");
    return (l->kind == TY_ANY || r->kind == TY_ANY) ? typeAny() : typeInt();
  case OP_NODE_SUB:
  case OP_NODE_MUL:
  case OP_NODE_DIV:
  case OP_NODE_MOD:
  case OP_NODE_BITAND:
  case OP_NODE_BITOR:
  case OP_NODE_BITXOR:
  case OP_NODE_SHL:
  case OP_NODE_SHR:
    requireInt(l, node->line, "left operand");
    requireInt(r, node->line, "right operand");
    return typeInt();
  case OP_NODE_LESS:
  case OP_NODE_GREATER:
    requireInt(l, node->line, "left operand of comparison");
    requireInt(r, node->line, "right operand of comparison");
    return typeBool();
  case OP_NODE_EQUAL:
    // `==` works on any pair of types (mismatched types are simply not equal at
    // runtime), so there is nothing to reject; the result is always bool.
    return typeBool();
  default:
    return typeAny();
  }
}

static Type *checkCall(Node *node) {
  Type *calleeType = checkExpr(node->as.call.callee);
  // Check the arguments regardless, so errors inside them are still reported.
  Type *argTypes[256];
  int argCount = node->as.call.argCount;
  for (int i = 0; i < argCount; i++)
    argTypes[i] = checkExpr(node->as.call.args[i]);

  if (calleeType->kind == TY_ANY)
    return typeAny(); // dynamic callee: defer all checks to runtime

  if (calleeType->kind != TY_FUNCTION) {
    char msg[96];
    snprintf(msg, sizeof(msg), "cannot call a value of type %s",
             typeName(calleeType));
    typeError(node->line, msg);
    return typeAny();
  }

  // Static arity check — caught before the program runs.
  if (calleeType->fn.paramCount != argCount) {
    char msg[96];
    snprintf(msg, sizeof(msg), "function expects %d arguments but got %d",
             calleeType->fn.paramCount, argCount);
    typeError(node->line, msg);
    return calleeType->fn.returnType;
  }

  // Argument-type checks, position by position (gradual: any matches anything).
  for (int i = 0; i < argCount; i++) {
    if (!compatible(calleeType->fn.params[i], argTypes[i])) {
      char msg[128];
      snprintf(msg, sizeof(msg), "argument %d expects %s but got %s", i + 1,
               typeName(calleeType->fn.params[i]), typeName(argTypes[i]));
      typeError(node->line, msg);
    }
  }
  return calleeType->fn.returnType;
}

// Compute the type of an expression node, reporting any errors along the way.
static Type *checkExpr(Node *node) {
  switch (node->type) {
  case NODE_INT:
    return typeInt();
  case NODE_BOOL:
    return typeBool();
  case NODE_NIL:
    return typeNil();
  case NODE_STRING:
    return typeStr();
  case NODE_VAR_GET:
    return lookupSymbol(node->as.name);
  case NODE_ASSIGN: {
    Type *valueType = checkExpr(node->as.var.value);
    Type *varType = lookupSymbol(node->as.var.name);
    if (!compatible(varType, valueType)) {
      char msg[128];
      snprintf(msg, sizeof(msg), "cannot assign %s to variable of type %s",
               typeName(valueType), typeName(varType));
      typeError(node->line, msg);
    }
    return varType; // assignment yields the variable's (declared) type
  }
  case NODE_UNARY:
    if (node->as.unary.op == OP_NODE_NEGATE) {
      requireInt(checkExpr(node->as.unary.operand), node->line, "operand of '-'");
      return typeInt();
    }
    if (node->as.unary.op == OP_NODE_BITNOT) {
      requireInt(checkExpr(node->as.unary.operand), node->line, "operand of '~'");
      return typeInt();
    }
    // `!` accepts anything (truthiness) and yields bool.
    checkExpr(node->as.unary.operand);
    return typeBool();
  case NODE_BINARY:
    return checkBinary(node);
  case NODE_LOGICAL:
    // `and`/`or` return the deciding operand, whose type we can't pin down
    // statically, so the result is `any`. We still walk both sides for errors.
    checkExpr(node->as.logical.left);
    checkExpr(node->as.logical.right);
    return typeAny();
  case NODE_CALL:
    return checkCall(node);
  case NODE_INVOKE:
    // Builtin methods aren't part of the gradual type system (their signatures
    // live in C), so a method call is `any`: we walk the receiver and arguments
    // for errors inside them, but don't check the method itself — the VM does.
    checkExpr(node->as.invoke.receiver);
    for (int i = 0; i < node->as.invoke.argCount; i++)
      checkExpr(node->as.invoke.args[i]);
    return typeAny();
  case NODE_ARRAY: {
    // Infer the element type from the literal: if every element shares one kind,
    // that is the element type; a mix (or any) makes it `[any]`. We still check
    // every element for inner errors. An empty literal is `[any]`.
    int n = node->as.array.count;
    Type *elem = n > 0 ? checkExpr(node->as.array.elements[0]) : typeAny();
    for (int i = 1; i < n; i++) {
      Type *t = checkExpr(node->as.array.elements[i]);
      if (t->kind != elem->kind)
        elem = typeAny();
    }
    return typeArray(elem);
  }
  case NODE_MAP: {
    // Infer key and value types independently, each homogeneous-or-`any`, exactly
    // like array element inference. An empty literal is `{any: any}`.
    int n = node->as.map.count;
    Type *keyT = n > 0 ? checkExpr(node->as.map.keys[0]) : typeAny();
    Type *valT = n > 0 ? checkExpr(node->as.map.values[0]) : typeAny();
    for (int i = 1; i < n; i++) {
      if (checkExpr(node->as.map.keys[i])->kind != keyT->kind)
        keyT = typeAny();
      if (checkExpr(node->as.map.values[i])->kind != valT->kind)
        valT = typeAny();
    }
    return typeMap(keyT, valT);
  }
  case NODE_INDEX_GET: {
    Type *obj = checkExpr(node->as.index.object);
    Type *idx = checkExpr(node->as.index.index);
    if (obj->kind == TY_ARRAY) {
      if (idx->kind != TY_INT && idx->kind != TY_ANY)
        typeError(node->line, "array index must be int");
      return obj->element; // a known element type — real static information
    }
    if (obj->kind == TY_MAP) {
      if (!compatible(obj->map.key, idx)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "map key should be %s but got %s",
                 typeName(obj->map.key), typeName(idx));
        typeError(node->line, msg);
      }
      return obj->map.value; // the map's value type
    }
    if (obj->kind != TY_ANY) {
      char msg[96];
      snprintf(msg, sizeof(msg), "cannot index a value of type %s", typeName(obj));
      typeError(node->line, msg);
    }
    return typeAny();
  }
  case NODE_INDEX_SET: {
    Type *obj = checkExpr(node->as.index.object);
    Type *idx = checkExpr(node->as.index.index);
    Type *val = checkExpr(node->as.index.value);
    if (obj->kind == TY_ARRAY) {
      if (idx->kind != TY_INT && idx->kind != TY_ANY)
        typeError(node->line, "array index must be int");
      if (!compatible(obj->element, val)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "cannot store %s into an array of %s",
                 typeName(val), typeName(obj->element));
        typeError(node->line, msg);
      }
    } else if (obj->kind == TY_MAP) {
      if (!compatible(obj->map.key, idx)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "map key should be %s but got %s",
                 typeName(obj->map.key), typeName(idx));
        typeError(node->line, msg);
      }
      if (!compatible(obj->map.value, val)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "cannot store %s as a %s value", typeName(val),
                 typeName(obj->map.value));
        typeError(node->line, msg);
      }
    } else if (obj->kind != TY_ANY) {
      char msg[96];
      snprintf(msg, sizeof(msg), "cannot index a value of type %s", typeName(obj));
      typeError(node->line, msg);
    }
    return val; // an index-assignment yields the assigned value
  }
  case NODE_FIELD_GET: {
    Type *obj = resolve(checkExpr(node->as.field.object), node->line);
    if (obj->kind == TY_STRUCT) {
      for (int i = 0; i < obj->strct.fieldCount; i++)
        if (obj->strct.fieldNames[i] == node->as.field.field)
          return resolve(obj->strct.fieldTypes[i], node->line);
      char msg[96];
      snprintf(msg, sizeof(msg), "%s has no field '%s'", typeName(obj),
               node->as.field.field->chars);
      typeError(node->line, msg);
      return typeAny();
    }
    if (obj->kind != TY_ANY) {
      char msg[96];
      snprintf(msg, sizeof(msg), "cannot read a field of type %s", typeName(obj));
      typeError(node->line, msg);
    }
    return typeAny();
  }
  case NODE_FIELD_SET: {
    Type *obj = resolve(checkExpr(node->as.field.object), node->line);
    Type *val = checkExpr(node->as.field.value);
    if (obj->kind == TY_STRUCT) {
      for (int i = 0; i < obj->strct.fieldCount; i++)
        if (obj->strct.fieldNames[i] == node->as.field.field) {
          Type *ft = resolve(obj->strct.fieldTypes[i], node->line);
          if (!compatible(ft, val)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "cannot store %s into field '%s' of type %s",
                     typeName(val), node->as.field.field->chars, typeName(ft));
            typeError(node->line, msg);
          }
          return val;
        }
      char msg[96];
      snprintf(msg, sizeof(msg), "%s has no field '%s'", typeName(obj),
               node->as.field.field->chars);
      typeError(node->line, msg);
    } else if (obj->kind != TY_ANY) {
      char msg[96];
      snprintf(msg, sizeof(msg), "cannot set a field of type %s", typeName(obj));
      typeError(node->line, msg);
    }
    return val;
  }
  default:
    return typeAny(); // statement nodes shouldn't appear in expression position
  }
}

// Build a function's Type from its annotations (used both to register it and to
// check its body). The annotations are already full Type* on the AST, so we just
// copy the param-type pointers into an array and hand them to typeFunction (which
// takes ownership of that array — the type arena frees it).
static Type *functionTypeOf(Node *fun) {
  int n = fun->as.fun.paramCount;
  Type **params = n > 0 ? malloc(sizeof(Type *) * n) : NULL;
  if (n > 0 && params == NULL) {
    fprintf(stderr, "cnano: out of memory building function type\n");
    exit(70);
  }
  for (int i = 0; i < n; i++)
    params[i] = resolve(fun->as.fun.paramTypes[i], fun->line);
  return typeFunction(params, n, resolve(fun->as.fun.returnType, fun->line));
}

static void checkFunction(Node *node) {
  Type *fnType = functionTypeOf(node);
  // Bind the function's own name in the CURRENT scope before checking the body,
  // so the function can refer to itself (recursion).
  declareSymbol(node->as.fun.name, fnType);

  // Check the body in a fresh scope, with parameters bound to their types and the
  // expected return type recorded for `return` validation.
  Type *savedReturn = checker.currentReturnType;
  checker.currentReturnType = resolve(node->as.fun.returnType, node->line);
  beginScope();
  for (int i = 0; i < node->as.fun.paramCount; i++)
    declareSymbol(node->as.fun.params[i],
                  resolve(node->as.fun.paramTypes[i], node->line));
  Program *body = node->as.fun.body;
  for (int i = 0; i < body->count; i++)
    checkStatement(body->statements[i]);
  endScope();
  checker.currentReturnType = savedReturn;
}

// Type-check a struct METHOD body, with `self` bound to the struct type so the
// body can use `self.field`. Mirrors checkFunction but adds the receiver.
static void checkMethod(Type *structType, Node *m) {
  Type *savedReturn = checker.currentReturnType;
  checker.currentReturnType = resolve(m->as.fun.returnType, m->line);
  beginScope();
  declareSymbol(copyString("self", 4), structType);
  for (int i = 0; i < m->as.fun.paramCount; i++)
    declareSymbol(m->as.fun.params[i], resolve(m->as.fun.paramTypes[i], m->line));
  Program *body = m->as.fun.body;
  for (int i = 0; i < body->count; i++)
    checkStatement(body->statements[i]);
  endScope();
  checker.currentReturnType = savedReturn;
}

// FLOW NARROWING: if `cond` is a nil-guard on a nullable variable, return that
// variable's symbol slot and set *nonNilInThen to which branch proves it non-nil
// (`x != nil` -> then; `x == nil` -> else). Recall `!=` desugars to `!(==)`. This
// lets the checker treat a `T?` as `T` inside the guarded branch — the key to
// using optionals without casts. Returns -1 when no such guard applies.
static int nilGuardSlot(Node *cond, bool *nonNilInThen) {
  Node *eq = NULL;
  bool inThen = false;
  if (cond->type == NODE_UNARY && cond->as.unary.op == OP_NODE_NOT &&
      cond->as.unary.operand->type == NODE_BINARY &&
      cond->as.unary.operand->as.binary.op == OP_NODE_EQUAL) {
    eq = cond->as.unary.operand; // x != nil  (i.e. !(x == nil))
    inThen = true;
  } else if (cond->type == NODE_BINARY && cond->as.binary.op == OP_NODE_EQUAL) {
    eq = cond; // x == nil
    inThen = false;
  }
  if (eq == NULL)
    return -1;
  Node *l = eq->as.binary.left, *r = eq->as.binary.right;
  Node *var = (l->type == NODE_VAR_GET && r->type == NODE_NIL)   ? l
              : (r->type == NODE_VAR_GET && l->type == NODE_NIL) ? r
                                                                 : NULL;
  if (var == NULL)
    return -1;
  for (int i = checker.symbolCount - 1; i >= 0; i--)
    if (checker.symbols[i].name == var->as.name) {
      if (checker.symbols[i].type->kind != TY_NULLABLE)
        return -1; // only narrowing a nullable means anything
      *nonNilInThen = inThen;
      return i;
    }
  return -1;
}

static void checkStatement(Node *node) {
  switch (node->type) {
  case NODE_PRINT:
  case NODE_EXPR_STMT:
    checkExpr(node->as.stmt.expr);
    break;
  case NODE_VAR_DECL: {
    Type *valueType = checkExpr(node->as.var.value);
    Type *declared = resolve(node->as.var.declaredType, node->line);
    if (declared->kind != TY_ANY && !compatible(declared, valueType)) {
      char msg[128];
      snprintf(msg, sizeof(msg), "initialiser is %s but variable is declared %s",
               typeName(valueType), typeName(declared));
      typeError(node->line, msg);
    }
    // The variable's static type is its annotation if given, else the (possibly
    // inferred) type of its initialiser — a tiny bit of type INFERENCE.
    declareSymbol(node->as.var.name,
                  declared->kind != TY_ANY ? declared : valueType);
    break;
  }
  case NODE_BLOCK: {
    beginScope();
    Program *b = node->as.block;
    for (int i = 0; i < b->count; i++)
      checkStatement(b->statements[i]);
    endScope();
    break;
  }
  case NODE_IF: {
    checkExpr(node->as.ifStmt.condition);
    // Narrow a nullable variable inside whichever branch proves it non-nil,
    // temporarily overriding its symbol type and restoring it afterwards.
    bool inThen = false;
    int slot = nilGuardSlot(node->as.ifStmt.condition, &inThen);
    Type *saved = slot >= 0 ? checker.symbols[slot].type : NULL;
    Type *inner = saved ? saved->element : NULL;

    if (slot >= 0 && inThen)
      checker.symbols[slot].type = inner;
    checkStatement(node->as.ifStmt.then);
    if (slot >= 0)
      checker.symbols[slot].type = saved;

    if (node->as.ifStmt.otherwise != NULL) {
      if (slot >= 0 && !inThen)
        checker.symbols[slot].type = inner;
      checkStatement(node->as.ifStmt.otherwise);
      if (slot >= 0)
        checker.symbols[slot].type = saved;
    }
    break;
  }
  case NODE_WHILE:
    checkExpr(node->as.whileStmt.condition);
    checkStatement(node->as.whileStmt.body);
    if (node->as.whileStmt.increment != NULL)
      checkExpr(node->as.whileStmt.increment);
    break;
  case NODE_BREAK:
  case NODE_CONTINUE:
    break; // loop control carries no value to check
  case NODE_FUN:
    checkFunction(node);
    break;
  case NODE_STRUCT: {
    // Registered in the pre-pass; here validate field types resolve, then check
    // each method body with `self` bound to this struct's type.
    for (int i = 0; i < node->as.structDecl.fieldCount; i++)
      resolve(node->as.structDecl.fieldTypes[i], node->line);
    Type *structType = resolve(typeStructRef(node->as.structDecl.name), node->line);
    for (int i = 0; i < node->as.structDecl.methodCount; i++)
      checkMethod(structType, node->as.structDecl.methods[i]);
    break;
  }
  case NODE_THROW:
    checkExpr(node->as.stmt.expr); // any value may be thrown
    break;
  case NODE_TRY:
    checkStatement(node->as.tryStmt.body);
    beginScope();
    declareSymbol(node->as.tryStmt.catchName, typeAny()); // thrown values are dynamic
    checkStatement(node->as.tryStmt.handler);
    endScope();
    break;
  case NODE_RETURN: {
    Type *retType = node->as.ret.value ? checkExpr(node->as.ret.value) : typeNil();
    if (checker.currentReturnType != NULL &&
        !compatible(checker.currentReturnType, retType)) {
      char msg[128];
      snprintf(msg, sizeof(msg), "returning %s from a function declared to return %s",
               typeName(retType), typeName(checker.currentReturnType));
      typeError(node->line, msg);
    }
    break;
  }
  default:
    break;
  }
}

bool typecheckProgram(Program *program) {
  checker.symbolCount = 0;
  checker.scopeDepth = 0;
  checker.currentReturnType = NULL;
  checker.hadError = false;
  structCount = 0;

  // PASS 0a: register every struct's NAMED TYPE, so references to it (in field
  // types, function signatures, annotations) resolve — including forward and
  // mutually-recursive references. Field types stay as-is and are resolved
  // lazily at use, by which point every struct name is known.
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type == NODE_STRUCT)
      registerStruct(s->as.structDecl.name,
                     typeStruct(s->as.structDecl.name, s->as.structDecl.fieldNames,
                                s->as.structDecl.fieldTypes,
                                s->as.structDecl.fieldCount));
  }

  // PASS 0b: bind each struct NAME as a constructor value — a function from its
  // (resolved) field types to an instance of the struct. This is what makes
  // `Point(1, 2)` type-check (arity + argument types) and yield a `Point`. All
  // struct names are registered now, so field-type references resolve here.
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type != NODE_STRUCT)
      continue;
    // If the struct defines `init`, the constructor takes ITS parameters;
    // otherwise it takes one argument per field, in order.
    Node *init = NULL;
    for (int m = 0; m < s->as.structDecl.methodCount; m++)
      if (s->as.structDecl.methods[m]->as.fun.name == copyString("init", 4)) {
        init = s->as.structDecl.methods[m];
        break;
      }
    int n = init != NULL ? init->as.fun.paramCount : s->as.structDecl.fieldCount;
    Type **paramTypesArr =
        init != NULL ? init->as.fun.paramTypes : s->as.structDecl.fieldTypes;
    Type **params = n > 0 ? malloc(sizeof(Type *) * n) : NULL;
    if (n > 0 && params == NULL) {
      fprintf(stderr, "cnano: out of memory building a constructor type\n");
      exit(70);
    }
    for (int f = 0; f < n; f++)
      params[f] = resolve(paramTypesArr[f], s->line);
    Type *structType = resolve(typeStructRef(s->as.structDecl.name), s->line);
    declareSymbol(s->as.structDecl.name, typeFunction(params, n, structType));
  }

  // PASS 1: register every top-level function's TYPE before checking any bodies.
  // This is what lets a function call another that is declared LATER, and lets
  // mutually-recursive functions type-check each other. Forward declaration of
  // types is a standard move in any checker for a language with hoisted globals.
  for (int i = 0; i < program->count; i++) {
    Node *s = program->statements[i];
    if (s->type == NODE_FUN)
      declareSymbol(s->as.fun.name, functionTypeOf(s));
  }

  // PASS 2: check every statement. Top-level functions are already bound, so
  // checkFunction re-binding the same name just shadows harmlessly with an
  // equal type; its real job here is checking the body.
  for (int i = 0; i < program->count; i++)
    checkStatement(program->statements[i]);

  // Note: the type arena is NOT freed here. Annotation/function Type* live on the
  // AST and are needed by later passes (e.g. the native codegen reads them), so
  // the driver frees all types via freeTypes() once compilation is complete.
  return !checker.hadError;
}

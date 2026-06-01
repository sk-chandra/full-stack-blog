// ast.h — the Abstract Syntax Tree (AST).
//
// The parser does not emit bytecode directly. Instead it builds a tree that
// mirrors the *structure* of the expression. For `1 + 2 * 3` the tree is:
//
//         (+)
//        /   .
//       1    (*)
//           /   .
//          2     3
//
// (dots stand for the right branches: a backslash ending a // line would be
//  read as a line-continuation by the C preprocessor and warn under -Wall.)
//
// The tree captures precedence as SHAPE: multiplication sits lower, so it is
// evaluated first. Separating "understand the structure" (parser → AST) from
// "produce instructions" (compiler walks AST → bytecode) is the single biggest
// readability win in a compiler, and it is how almost every real compiler is
// organised. A one-pass compiler can skip the tree, but you lose the clean
// separation and the ability to analyse/optimise the program as data.
#ifndef CNANO_AST_H
#define CNANO_AST_H

#include "common.h"
#include "object.h" // ObjString, for string literals and variable names
#include "type.h"   // TypeKind, for optional type annotations on the AST

// cnano now has TWO categories of node, and the distinction is the single most
// important structural idea in language design:
//   * an EXPRESSION computes and yields a value   (1 + 2, a < b, true)
//   * a STATEMENT performs an action for its effect, yielding NO value
//     (print x;  or an expression evaluated only for its side effects)
// A program is a *sequence of statements*; each statement may contain
// expressions. We keep both in one Node type for simplicity, but the comments
// and the compiler treat the two categories differently — see compiler.c.
typedef enum {
  // --- expression nodes (yield a value onto the VM stack) ---
  NODE_INT,     // a literal integer
  NODE_FLOAT,   // a literal floating-point number
  NODE_BOOL,    // a literal `true` or `false`
  NODE_NIL,     // the literal `nil`
  NODE_STRING,  // a string literal (a heap ObjString)
  NODE_UNARY,   // a prefix operator applied to one child (e.g. -x, !x)
  NODE_BINARY,  // an operator with a left and right child (e.g. a + b, a < b)
  NODE_VAR_GET, // read a variable: yields its current value
  NODE_ASSIGN,  // `name = EXPR` : store EXPR into name, yields the value
  NODE_LOGICAL, // `a and b` / `a or b` : SHORT-CIRCUITS, so not a plain binary
  NODE_COND,    // `c ? a : b` : a conditional EXPRESSION (yields a or b)
  NODE_CALL,    // `callee(arg, arg, ...)` : call a function, yields its result
  NODE_INVOKE,  // `receiver.method(arg, ...)` : call a builtin method, yields result
  NODE_IS,      // `expr is TYPE` : runtime type test, yields bool
  NODE_ARRAY,   // `[a, b, c]` : an array literal, yields a new array
  NODE_MAP,     // `{k: v, ...}` : a map literal, yields a new map
  NODE_INDEX_GET, // `obj[i]` : read element i of obj
  NODE_INDEX_SET, // `obj[i] = v` : store v at element i, yields v
  NODE_FIELD_GET, // `obj.field` : read a struct field
  NODE_FIELD_SET, // `obj.field = v` : write a struct field, yields v
  // --- statement nodes (performed for effect, yield nothing) ---
  NODE_STRUCT,    // `struct Name { field: T, ... }` — a struct declaration
  NODE_ENUM,      // `enum Name { A, B, C }` — a set of named constant members
  NODE_THROW,     // `throw EXPR;` — raise a value
  NODE_TRY,       // `try { ... } catch (e) { ... }` — guard a block
  NODE_BREAK,     // `break;` — exit the innermost loop
  NODE_CONTINUE,  // `continue;` — next iteration of the innermost loop
  NODE_PRINT,      // `print EXPR;` — evaluate EXPR and print it
  NODE_EXPR_STMT,  // `EXPR;` — evaluate EXPR, then discard its value
  NODE_VAR_DECL,   // `let name = EXPR;` — declare a variable (global or local)
  NODE_BLOCK,      // `{ ... }` — a new lexical scope holding more statements
  NODE_IF,         // `if (c) then [else otherwise]`
  NODE_WHILE,      // `while (c) body`
  NODE_FUN,        // `fn name(params) { body }` — a function declaration
  NODE_RETURN,     // `return [EXPR];` — return from the enclosing function
  NODE_IMPORT,     // `import "path";` — splice another file's top-level decls
  NODE_MATCH,      // `match (s) { ... }` — wraps its lowered if-chain plus arm
                   // descriptors, so the checker can test for exhaustiveness
} NodeType;

// The operator carried by unary/binary nodes. Keeping this separate from the
// lexer's TokenType means the compiler switches over a small, meaningful set
// rather than over raw token kinds.
//
// Note what is and isn't here: we have NEGATE, NOT, ADD..DIV, plus the three
// PRIMITIVE comparisons EQUAL, LESS, GREATER. There is deliberately no
// NOT_EQUAL/LESS_EQUAL/GREATER_EQUAL operator — the parser *desugars* those into
// combinations of the primitives (see parser.c). Fewer operators here means
// fewer cases in the compiler and fewer opcodes in the VM.
typedef enum {
  OP_NODE_NEGATE,  // -x  (arithmetic)
  OP_NODE_NOT,     // !x  (logical)
  OP_NODE_ADD,
  OP_NODE_SUB,
  OP_NODE_MUL,
  OP_NODE_DIV,
  OP_NODE_MOD,
  OP_NODE_BITAND, // &
  OP_NODE_BITOR,  // |
  OP_NODE_BITXOR, // ^
  OP_NODE_SHL,    // <<
  OP_NODE_SHR,    // >>
  OP_NODE_BITNOT, // ~  (unary)
  OP_NODE_EQUAL,   // ==
  OP_NODE_LESS,    // <
  OP_NODE_GREATER, // >
} NodeOp;

// One `match` value-arm, described just enough for the exhaustiveness check:
// either it names an enum member (`Color.Red`), a bool literal, or "something
// else" we can't reason about for coverage. (Type arms and the body live in the
// lowered chain; this is only the metadata the checker needs.)
typedef enum { MATCH_ARM_ENUM, MATCH_ARM_BOOL, MATCH_ARM_TYPE, MATCH_ARM_OTHER } MatchArmKind;
typedef struct {
  MatchArmKind kind;
  struct ObjString *enumName; // MATCH_ARM_ENUM: the `Color` in `Color.Red`
  struct ObjString *member;   // MATCH_ARM_ENUM: the `Red`
  bool boolVal;               // MATCH_ARM_BOOL: which literal
  struct Type *type;          // MATCH_ARM_TYPE: the `T` in an `is T` arm
} MatchArm;

// Forward declaration: a Node can contain a Program (a block's body), but
// Program is defined further down in terms of Node. Naming it here breaks the
// cycle so the union member below can hold a `struct Program *`.
struct Program;

// A "tagged union": the `type` field tells you which arm of the union `as` is
// valid. This is the idiomatic C way to represent "one of several shapes" and
// is exactly how dynamically typed values and AST nodes are stored in real VMs.
typedef struct Node {
  NodeType type;
  int line; // source line, threaded through for runtime/compile error messages
  union {
    // NODE_INT carries an integer; NODE_BOOL carries a bool; NODE_NIL carries
    // nothing. We store the raw C payloads here (not a full tagged Value) and
    // let the compiler wrap them into Values — keeping the AST independent of the
    // runtime value representation.
    int64_t intValue;
    double floatValue;      // NODE_FLOAT: the literal double
    bool boolValue;
    ObjString *stringValue; // NODE_STRING: the interned string literal
    // Variable name for NODE_VAR_GET. The name is an interned ObjString so the
    // compiler can use it directly as a hash-table key.
    ObjString *name;
    struct {
      NodeOp op;
      struct Node *operand;
    } unary;
    struct {
      NodeOp op;
      struct Node *left;
      struct Node *right;
    } binary;
    // Both statement kinds wrap exactly one expression child: print evaluates
    // then prints it; an expression statement evaluates then discards it.
    struct {
      struct Node *expr;
    } stmt;
    // NODE_ASSIGN and NODE_VAR_DECL: a variable name plus the value expression.
    // Assignment is an expression (yields the value); declaration is a statement.
    // `declaredType` is the optional `: T` annotation on a `let` — a full Type*
    // now (so `[int]`, `{str: int}` are expressible), `typeAny()` when omitted
    // (the gradual default). Unused for NODE_ASSIGN.
    struct {
      ObjString *name;
      struct Node *value;
      Type *declaredType;
      bool isConst; // declared with `const` — reassignment is a compile error
    } var;
    // NODE_BLOCK: a brace-delimited sequence of statements forming a new scope.
    // We reuse the Program container (a growable Node* list) — a block is, after
    // all, just a nested program with its own scope.
    struct Program *block;
    // NODE_LOGICAL: `and`/`or`. isAnd selects which; both short-circuit.
    struct {
      bool isAnd;
      struct Node *left;
      struct Node *right;
    } logical;
    // NODE_IF: a condition, the `then` branch, and an optional `else` branch
    // (otherwise may be NULL). Branches are statements (often blocks).
    struct {
      struct Node *condition;
      struct Node *then;
      struct Node *otherwise; // NULL if there is no else
    } ifStmt;
    // NODE_WHILE: a condition, a loop body, and an optional `increment` run after
    // the body each iteration (NULL for a plain `while`; set by the `for` desugar
    // so that `continue` runs the step rather than skipping it).
    struct {
      struct Node *condition;
      struct Node *body;
      struct Node *increment; // an expression run each iteration; may be NULL
      bool isDoWhile;         // `do { } while (c)`: run the body before testing c
    } whileStmt;
    // NODE_CALL: the expression being called plus a list of argument expressions.
    struct {
      struct Node *callee;
      struct Node **args; // heap array of argument expression nodes
      int argCount;
    } call;
    // NODE_INVOKE: `receiver.method(args)`. Like a call, but the callee is a named
    // method looked up on the receiver's type at runtime rather than a value.
    struct {
      struct Node *receiver;
      ObjString *method; // the method name (interned)
      struct Node **args;
      int argCount;
    } invoke;
    // NODE_ARRAY: an array literal `[e0, e1, ...]`, built at runtime into a fresh
    // array object. Same heap-array-of-children shape as a call's arguments.
    struct {
      struct Node **elements;
      int count;
    } array;
    // NODE_MAP: a map literal `{k0: v0, ...}`. Keys and values are kept in two
    // parallel heap arrays, both of length `count`.
    struct {
      struct Node **keys;
      struct Node **values;
      int count;
    } map;
    // NODE_INDEX_GET (`obj[index]`) and NODE_INDEX_SET (`obj[index] = value`).
    // Index-set is produced by assignment() when an index expression is the
    // l-value; its `value` is NULL for the get form.
    struct {
      struct Node *object;
      struct Node *index;
      struct Node *value; // NODE_INDEX_SET only; NULL for NODE_INDEX_GET
    } index;
    // NODE_FIELD_GET (`obj.field`) and NODE_FIELD_SET (`obj.field = value`).
    struct {
      struct Node *object;
      ObjString *field;   // interned field name
      struct Node *value; // NODE_FIELD_SET only; NULL for the get form
    } field;
    // NODE_IS: `expr is type` — a runtime test of expr's type.
    struct {
      struct Node *expr;
      Type *type;
    } isTest;
    // NODE_STRUCT: a `struct` declaration. Field names are interned; field types
    // are full Type* (typeAny() if a field is unannotated).
    struct {
      ObjString *name;
      ObjString **fieldNames;
      Type **fieldTypes;
      int fieldCount;
      struct Node **methods; // NODE_FUN declarations inside the struct body
      int methodCount;
    } structDecl;
    // NODE_ENUM: an `enum Name { A, B, C }` declaration. Just a name and the
    // ordered list of (interned) member names — enums carry no data of their own.
    struct {
      ObjString *name;
      ObjString **memberNames;
      int memberCount;
    } enumDecl;
    // NODE_FUN: a function declaration. params holds the parameter NAMES (interned
    // ObjStrings); paramTypes the parallel `: T` annotations (typeAny() if
    // omitted); returnType the `: T` after the parameter list (typeAny() if
    // omitted); body the block of statements.
    struct {
      ObjString *name;
      ObjString **params;   // heap array of parameter names
      Type **paramTypes;    // parallel heap array of annotation Type* (typeAny default)
      int paramCount;
      Type *returnType;
      struct Program *body;
    } fun;
    // NODE_RETURN: the optional return value (NULL for a bare `return;`).
    struct {
      struct Node *value;
    } ret;
    // NODE_MATCH: the lowered if-chain (`body`, what the backends compile) plus
    // metadata for the exhaustiveness check — the subject variable (NULL unless
    // it is a plain variable) and one descriptor per value arm.
    struct {
      struct Node *body;
      struct ObjString *subjectName; // NULL => don't check exhaustiveness
      MatchArm *arms;                // heap array, owned by the node
      int armCount;
      bool hasDefault;
    } matchStmt;
    // NODE_TRY: a guarded block, a catch variable name, and the catch block.
    struct {
      struct Node *body;     // a NODE_BLOCK
      ObjString *catchName;  // the variable bound to the thrown value
      struct Node *handler;  // a NODE_BLOCK
    } tryStmt;
  } as;
} Node;

// A whole program: a growable list of top-level statement nodes, executed in
// order. This is the same dynamic-array pattern as ValueArray/Chunk — count,
// capacity, double-on-full. Separating "the program" from "a node" keeps the
// recursive Node type clean while still letting the parser produce many
// statements.
typedef struct Program {
  int count;
  int capacity;
  Node **statements; // array of owned Node* (each a statement)
} Program;

void initProgram(Program *program);
void freeProgram(Program *program); // frees every statement, then the array
void writeProgram(Program *program, Node *statement);

// Constructors. Each allocates a node on the heap and fills it in. The parser
// owns these; freeNode walks the tree and releases the whole thing.
Node *newInt(int64_t value, int line);
Node *newFloat(double value, int line);
Node *newBool(bool value, int line);
Node *newNil(int line);
Node *newUnary(NodeOp op, Node *operand, int line);
Node *newBinary(NodeOp op, Node *left, Node *right, int line);
Node *newString(ObjString *value, int line);
Node *newVarGet(ObjString *name, int line);
Node *newAssign(ObjString *name, Node *value, int line);
Node *newPrint(Node *expr, int line);
Node *newExprStmt(Node *expr, int line);
Node *newVarDecl(ObjString *name, Node *value, Type *declaredType, int line);
Node *newBlock(Program *block, int line); // takes ownership of `block`
Node *newLogical(bool isAnd, Node *left, Node *right, int line);
Node *newIf(Node *condition, Node *then, Node *otherwise, int line);
// `c ? a : b` — a conditional expression. Shares NODE_IF's three-child shape, but
// its branches are EXPRESSIONS and it yields a value. `otherwise` is never NULL.
Node *newCond(Node *condition, Node *thenExpr, Node *elseExpr, int line);
Node *newWhile(Node *condition, Node *body, int line);
// `do { body } while (c);` — like a while loop but the body runs once before the
// first test. Reuses the NODE_WHILE node with its isDoWhile flag set.
Node *newDoWhile(Node *condition, Node *body, int line);
// Takes ownership of the `args` array (freed by freeNode).
Node *newCall(Node *callee, Node **args, int argCount, int line);
// `receiver.method(args)`. Takes ownership of the `args` array.
Node *newInvoke(Node *receiver, ObjString *method, Node **args, int argCount,
                int line);
// `expr is type` — a runtime type test.
Node *newIs(Node *expr, Type *type, int line);
// `[e0, e1, ...]`. Takes ownership of the `elements` array.
Node *newArray(Node **elements, int count, int line);
// `{k0: v0, ...}`. Takes ownership of both the `keys` and `values` arrays.
Node *newMap(Node **keys, Node **values, int count, int line);
// `obj[index]` (a read) and `obj[index] = value` (a write).
Node *newIndexGet(Node *object, Node *index, int line);
Node *newIndexSet(Node *object, Node *index, Node *value, int line);

// `obj.field` (a read) and `obj.field = value` (a write).
Node *newFieldGet(Node *object, ObjString *field, int line);
Node *newFieldSet(Node *object, ObjString *field, Node *value, int line);

// `struct Name { ... }`. Takes ownership of the fieldNames and fieldTypes arrays.
Node *newStructDecl(ObjString *name, ObjString **fieldNames, Type **fieldTypes,
                    int fieldCount, int line);

// `enum Name { A, B, ... }`. Takes ownership of the memberNames array.
Node *newEnumDecl(ObjString *name, ObjString **memberNames, int memberCount,
                  int line);

// `throw EXPR;` and `try { body } catch (name) { handler }`.
Node *newThrow(Node *value, int line);
Node *newTry(Node *body, ObjString *catchName, Node *handler, int line);

// `break;` and `continue;` — loop control (no children).
Node *newBreak(int line);
Node *newContinue(int line);

// Deep-copy a PURE expression (literals, variable reads, and index reads built
// from those). Returns NULL for anything that could have a side effect (calls,
// assignments, ...). Used to desugar compound assignment `a[i] += e` into
// `a[i] = a[i] + e` without evaluating — or freeing — the target twice.
Node *cloneExpr(Node *node);
// Takes ownership of `params`, `paramTypes`, and `body`.
Node *newFun(ObjString *name, ObjString **params, Type **paramTypes,
             int paramCount, Type *returnType, Program *body, int line);
Node *newReturn(Node *value, int line); // value may be NULL
// `match` wrapper. Takes ownership of `body` and the `arms` array. `subjectName`
// (VM-owned, may be NULL) and arm enum/member names are borrowed, not freed.
Node *newMatch(Node *body, ObjString *subjectName, MatchArm *arms, int armCount,
               bool hasDefault, int line);
// `import "path";` — the path is an interned ObjString (VM-owned, not freed here).
Node *newImport(ObjString *path, int line);
void freeNode(Node *node);

#endif // CNANO_AST_H

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
  NODE_BOOL,    // a literal `true` or `false`
  NODE_NIL,     // the literal `nil`
  NODE_STRING,  // a string literal (a heap ObjString)
  NODE_UNARY,   // a prefix operator applied to one child (e.g. -x, !x)
  NODE_BINARY,  // an operator with a left and right child (e.g. a + b, a < b)
  NODE_VAR_GET, // read a variable: yields its current value
  NODE_ASSIGN,  // `name = EXPR` : store EXPR into name, yields the value
  NODE_LOGICAL, // `a and b` / `a or b` : SHORT-CIRCUITS, so not a plain binary
  NODE_CALL,    // `callee(arg, arg, ...)` : call a function, yields its result
  // --- statement nodes (performed for effect, yield nothing) ---
  NODE_PRINT,      // `print EXPR;` — evaluate EXPR and print it
  NODE_EXPR_STMT,  // `EXPR;` — evaluate EXPR, then discard its value
  NODE_VAR_DECL,   // `let name = EXPR;` — declare a variable (global or local)
  NODE_BLOCK,      // `{ ... }` — a new lexical scope holding more statements
  NODE_IF,         // `if (c) then [else otherwise]`
  NODE_WHILE,      // `while (c) body`
  NODE_FUN,        // `fn name(params) { body }` — a function declaration
  NODE_RETURN,     // `return [EXPR];` — return from the enclosing function
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
  OP_NODE_EQUAL,   // ==
  OP_NODE_LESS,    // <
  OP_NODE_GREATER, // >
} NodeOp;

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
    // NODE_WHILE: a condition and a loop body.
    struct {
      struct Node *condition;
      struct Node *body;
    } whileStmt;
    // NODE_CALL: the expression being called plus a list of argument expressions.
    struct {
      struct Node *callee;
      struct Node **args; // heap array of argument expression nodes
      int argCount;
    } call;
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
Node *newWhile(Node *condition, Node *body, int line);
// Takes ownership of the `args` array (freed by freeNode).
Node *newCall(Node *callee, Node **args, int argCount, int line);
// Takes ownership of `params`, `paramTypes`, and `body`.
Node *newFun(ObjString *name, ObjString **params, Type **paramTypes,
             int paramCount, Type *returnType, Program *body, int line);
Node *newReturn(Node *value, int line); // value may be NULL
void freeNode(Node *node);

#endif // CNANO_AST_H

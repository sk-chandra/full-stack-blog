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

typedef enum {
  NODE_NUMBER, // a literal integer
  NODE_UNARY,  // a prefix operator applied to one child (e.g. -x)
  NODE_BINARY, // an operator with a left and right child (e.g. a + b)
} NodeType;

// The operator carried by unary/binary nodes. Keeping this separate from the
// lexer's TokenType means the compiler switches over a small, meaningful set
// rather than over raw token kinds.
typedef enum {
  OP_NODE_NEGATE,
  OP_NODE_ADD,
  OP_NODE_SUB,
  OP_NODE_MUL,
  OP_NODE_DIV,
} NodeOp;

// A "tagged union": the `type` field tells you which arm of the union `as` is
// valid. This is the idiomatic C way to represent "one of several shapes" and
// is exactly how dynamically typed values and AST nodes are stored in real VMs.
typedef struct Node {
  NodeType type;
  int line; // source line, threaded through for runtime/compile error messages
  union {
    struct {
      Value value;
    } number;
    struct {
      NodeOp op;
      struct Node *operand;
    } unary;
    struct {
      NodeOp op;
      struct Node *left;
      struct Node *right;
    } binary;
  } as;
} Node;

// Constructors. Each allocates a node on the heap and fills it in. The parser
// owns these; freeNode walks the tree and releases the whole thing.
Node *newNumber(Value value, int line);
Node *newUnary(NodeOp op, Node *operand, int line);
Node *newBinary(NodeOp op, Node *left, Node *right, int line);
void freeNode(Node *node);

#endif // CNANO_AST_H

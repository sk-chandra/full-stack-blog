#include <stdio.h>
#include <stdlib.h>
#include <string.h> // memcmp — for recognising type-name tokens

#include "ast.h"
#include "lexer.h"
#include "module.h" // moduleFormatLine / moduleLocalLine — name the file in errors
#include "object.h" // copyString — interns identifier and string-literal text
#include "parser.h"

// The grammar we implement, written in EBNF. Each rule below becomes a function.
//
// A program is now a SEQUENCE OF STATEMENTS, not a single expression. The top of
// the grammar is therefore about statements; expressions sit underneath:
//
//   program     -> declaration* EOF ;
//   declaration -> funDecl | varDecl | statement ;
//   funDecl     -> "fn" IDENTIFIER "(" parameters? ")" block ;
//   parameters  -> IDENTIFIER ( "," IDENTIFIER )* ;
//   varDecl     -> "let" IDENTIFIER "=" expression ";" ;
//   statement   -> printStmt | ifStmt | whileStmt | forStmt | returnStmt
//                | block | exprStmt ;
//   returnStmt  -> "return" expression? ";" ;
//   ifStmt      -> "if" "(" expression ")" statement ( "else" statement )? ;
//   whileStmt   -> "while" "(" expression ")" statement ;
//   forStmt     -> "for" "(" ( varDecl | exprStmt | ";" )
//                            expression? ";" expression? ")" statement ;
//   block       -> "{" declaration* "}" ;
//   printStmt   -> "print" expression ";" ;
//   exprStmt    -> expression ";" ;
//
// Lower expression rules bind tighter (higher precedence):
//
//   expression -> assignment ;
//   assignment -> IDENTIFIER "=" assignment | logic_or ;   // right-associative
//   logic_or   -> logic_and ( "or" logic_and )* ;          // short-circuits
//   logic_and  -> equality  ( "and" equality )* ;          // short-circuits
//
// ASSIGNMENT and the l-value problem: `a = 1` reads left-to-right, but the left
// side is a *target* (where to store), not a value to compute. The clean
// recursive-descent trick: parse the left side as an ordinary expression, then
// if a "=" follows, check that what we parsed is actually assignable (a bare
// variable) and rebuild it as an assignment. Assignment is right-associative
// (a = b = 1 means a = (b = 1)) and lowest precedence, so it sits at the very
// top of the expression grammar.
//   equality   -> comparison ( ( "==" | "!=" ) comparison )* ;
//   comparison -> term ( ( "<" | "<=" | ">" | ">=" ) term )* ;
//   term       -> factor ( ( "+" | "-" ) factor )* ;       // left-associative
//   factor     -> unary  ( ( "*" | "/" ) unary  )* ;       // left-associative
//   unary      -> ( "-" | "!" ) unary | call ;
//   call       -> primary ( "(" arguments? ")" )* ;   // postfix call(s)
//   arguments  -> expression ( "," expression )* ;
//   primary    -> INT | STRING | IDENTIFIER | "true" | "false" | "nil"
//               | "(" expression ")" ;
//
// Precedence falls out of the call chain: equality calls comparison calls term
// calls factor calls unary calls primary. Each new level we add sits ABOVE the
// arithmetic it should bind looser than — so `1 + 2 == 3` parses as
// `(1 + 2) == 3`, which is what people expect. Associativity falls out of the
// loops (`while`): we fold left-to-right, so 1-2-3 parses as (1-2)-3.
//
// DESUGARING: the grammar lists `!=`, `<=`, `>=`, but the AST has no operators
// for them. The parser rewrites them into the three primitives the rest of the
// pipeline supports:
//     a != b   becomes   !(a == b)
//     a <= b   becomes   !(a > b)
//     a >= b   becomes   !(a < b)
// This is "syntactic sugar": surface syntax that expands into a simpler core.
// Doing it once, here, means the compiler and VM never need to know these forms
// exist — a recurring strategy for keeping a language's CORE small while its
// SURFACE stays convenient.

typedef struct {
  Token current;  // the next token to consume (one-token lookahead)
  Token previous; // the most recently consumed token
  bool hadError;  // did ANY error occur during the whole parse?
  bool panicMode; // are we currently recovering from an error?
  bool fnSawYield; // did the function currently being parsed contain a `yield`?
} Parser;

static Parser parser;

// --- error reporting -------------------------------------------------------

// Render the source line `token` sits on, with a caret underline beneath the
// token — the kind of diagnostic real compilers print:
//
//   [line 12] Error at 'fn': Expect a type after ':'.
//       12 | fn apply(stack: [float], op: fn): float {
//          |                              ^^
//
// Degrades gracefully: a lexer ERROR token (whose text is a message, not a
// source slice) or EOF gets the line but no caret.
static void showSource(Token *token) {
  const char *src = lexerSource();
  if (src == NULL)
    return;
  // token->line is banded (file index * SPAN + local line); walk the current
  // file's buffer to the LOCAL line.
  int local = moduleLocalLine(token->line);
  const char *lineStart = src;
  for (int ln = 1; ln < local && *lineStart != '\0'; lineStart++)
    if (*lineStart == '\n')
      ln++;
  const char *lineEnd = lineStart;
  while (*lineEnd != '\0' && *lineEnd != '\n')
    lineEnd++;
  int lineLen = (int)(lineEnd - lineStart);
  fprintf(stderr, "  %4d | %.*s\n", local, lineLen, lineStart);

  // Underline the token only when its text is an actual slice of THIS line.
  if (token->type == TOKEN_ERROR || token->type == TOKEN_EOF ||
      token->start < lineStart || token->start > lineEnd)
    return;
  int col = (int)(token->start - lineStart);
  int caretLen = token->length > 0 ? token->length : 1;
  if (col + caretLen > lineLen)
    caretLen = lineLen - col;
  if (caretLen < 1)
    caretLen = 1;
  fprintf(stderr, "       | %*s", col, "");
  for (int i = 0; i < caretLen; i++)
    fputc('^', stderr);
  fputc('\n', stderr);
}

static void errorAt(Token *token, const char *message) {
  // panicMode suppresses the cascade of bogus errors that follows a real one,
  // until we resynchronise at a statement boundary (see synchronize()). Without
  // this, one mistake produces a wall of confusing messages. hadError stays set
  // for the whole parse so the caller knows compilation must be aborted.
  if (parser.panicMode)
    return;
  parser.panicMode = true;
  parser.hadError = true;
  char loc[128];
  moduleFormatLine(token->line, loc, sizeof(loc));
  fprintf(stderr, "[%s] Error", loc);
  if (token->type == TOKEN_EOF) {
    fprintf(stderr, " at end");
  } else if (token->type == TOKEN_ERROR) {
    // The lexer already put a message in the token's text.
  } else {
    fprintf(stderr, " at '%.*s'", token->length, token->start);
  }
  fprintf(stderr, ": %s\n", message);
  showSource(token);
}

// --- token cursor ----------------------------------------------------------

// Pull the next real token, surfacing any lexer error tokens as parse errors.
static void advance(void) {
  parser.previous = parser.current;
  for (;;) {
    parser.current = scanToken();
    if (parser.current.type != TOKEN_ERROR)
      break;
    errorAt(&parser.current, parser.current.start);
  }
}

// Consume the current token if it matches; otherwise report `message`.
static void consume(TokenType type, const char *message) {
  if (parser.current.type == type) {
    advance();
    return;
  }
  errorAt(&parser.current, message);
}

static bool check(TokenType type) { return parser.current.type == type; }

// If the current token is `type`, consume it and return true.
static bool match(TokenType type) {
  if (!check(type))
    return false;
  advance();
  return true;
}

// --- grammar rules (forward declarations, since they call each other) -------

static Node *expression(void);
static Node *assignment(void);
static Node *logicOr(void);
static Node *logicAnd(void);
static Node *finishFunction(ObjString *name, int line); // lambdas + fn decls
static Node *bitOr(void);
static Node *bitXor(void);
static Node *bitAnd(void);
static Node *equality(void);
static Node *comparison(void);
static Node *shift(void);
static Node *term(void);
static Node *factor(void);
static Node *unary(void);
static Node *primary(void);
static Type *parseType(void); // `is TYPE` and annotations both need this early

static Node *expression(void) { return assignment(); }

// Map a just-matched compound-assignment token to its arithmetic NodeOp.
static bool matchCompoundAssign(NodeOp *op) {
  if (match(TOKEN_PLUS_EQUAL)) { *op = OP_NODE_ADD; return true; }
  if (match(TOKEN_MINUS_EQUAL)) { *op = OP_NODE_SUB; return true; }
  if (match(TOKEN_STAR_EQUAL)) { *op = OP_NODE_MUL; return true; }
  if (match(TOKEN_SLASH_EQUAL)) { *op = OP_NODE_DIV; return true; }
  if (match(TOKEN_PERCENT_EQUAL)) { *op = OP_NODE_MOD; return true; }
  if (match(TOKEN_AMP_EQUAL)) { *op = OP_NODE_BITAND; return true; }
  if (match(TOKEN_PIPE_EQUAL)) { *op = OP_NODE_BITOR; return true; }
  if (match(TOKEN_CARET_EQUAL)) { *op = OP_NODE_BITXOR; return true; }
  if (match(TOKEN_LSHIFT_EQUAL)) { *op = OP_NODE_SHL; return true; }
  if (match(TOKEN_RSHIFT_EQUAL)) { *op = OP_NODE_SHR; return true; }
  return false;
}

// `c ? a : b` — the conditional expression. Sits just above assignment (lower
// than `or`), so the condition is a full logical expression and the branches may
// themselves be assignments or nested conditionals (right-associative `else`).
// The `?` here is unambiguous: a nullable type's `?` only ever appears in TYPE
// position (parsed by parseType), never in an ordinary expression.
static Node *ternary(void) {
  Node *cond = logicOr();
  if (match(TOKEN_QUESTION)) {
    int line = parser.previous.line;
    Node *thenExpr = expression(); // the middle is a full expression
    consume(TOKEN_COLON, "Expect ':' in a '?:' conditional expression.");
    Node *elseExpr = ternary(); // right-associative
    return newCond(cond, thenExpr, elseExpr, line);
  }
  return cond;
}

static Node *assignment(void) {
  // Parse the left-hand side as a normal expression first. It goes through the
  // logical and conditional operators, so `a or b` and `c ? a : b` are valid
  // l-value *bases* even though they are never valid assignment targets. A plain
  // `x` still comes back as a NODE_VAR_GET, so assignment targets are unaffected.
  Node *node = ternary();
  // A parse error below returns NULL (primary() reports it and advances). Don't
  // dereference it as an assignment target — propagate the NULL so the statement
  // loop resynchronises. (We only ever compile when the whole parse succeeded.)
  if (node == NULL)
    return NULL;

  // Compound assignment `target OP= rhs` desugars to `target = target OP rhs`.
  // We build it here so it works for both variable and index targets, reusing
  // the plain-assignment nodes — there is no dedicated opcode.
  NodeOp cop;
  if (matchCompoundAssign(&cop)) {
    int line = parser.previous.line;
    Node *rhs = assignment(); // right-associative, like '='

    if (node->type == NODE_VAR_GET) {
      // x OP= rhs  ->  x = (x OP rhs). A variable read has no side effect, so a
      // fresh read for the binary is safe; salvage the name and free the target.
      ObjString *name = node->as.name;
      Node *read = newVarGet(name, line);
      Node *combined = newBinary(cop, read, rhs, line);
      freeNode(node);
      return newAssign(name, combined, line);
    }

    if (node->type == NODE_INDEX_GET) {
      // a[i] OP= rhs  ->  a[i] = (a[i] OP rhs). Clone the (pure) object/index for
      // the STORE; reuse the original index-get node as the READ in the binary,
      // so neither side double-frees and side-effect-free targets evaluate alike.
      Node *objClone = cloneExpr(node->as.index.object);
      Node *idxClone = cloneExpr(node->as.index.index);
      if (objClone == NULL || idxClone == NULL) {
        errorAt(&parser.previous,
                "compound-assignment target is too complex; write it out as "
                "`a[i] = a[i] + x`.");
        freeNode(objClone);
        freeNode(idxClone);
        freeNode(rhs);
        return node;
      }
      Node *combined = newBinary(cop, node, rhs, line); // `node` is the read
      return newIndexSet(objClone, idxClone, combined, line);
    }

    if (node->type == NODE_FIELD_GET) {
      // a.f OP= rhs  ->  a.f = (a.f OP rhs). Clone the (pure) object for the
      // store; reuse the original field-get node as the read in the binary.
      Node *objClone = cloneExpr(node->as.field.object);
      if (objClone == NULL) {
        errorAt(&parser.previous,
                "compound-assignment target is too complex; write it out as "
                "`a.f = a.f + x`.");
        freeNode(rhs);
        return node;
      }
      ObjString *field = node->as.field.field;
      Node *combined = newBinary(cop, node, rhs, line); // `node` is the read
      return newFieldSet(objClone, field, combined, line);
    }

    errorAt(&parser.previous, "Invalid assignment target.");
    freeNode(rhs);
    return node;
  }

  // If a '=' follows, this was actually an assignment target.
  if (match(TOKEN_EQUAL)) {
    int line = parser.previous.line;
    Node *value = assignment(); // recurse right -> right-associative

    if (node->type == NODE_VAR_GET) {
      // Valid target: turn the "read x" we built into "assign to x". We salvage
      // the interned name, then free the throwaway VAR_GET node.
      ObjString *name = node->as.name;
      freeNode(node);
      return newAssign(name, value, line);
    }

    if (node->type == NODE_INDEX_GET) {
      // `obj[i] = value`: salvage the object and index out of the get node (NULL
      // them so freeing the wrapper leaves them intact) and rebuild as a set.
      Node *object = node->as.index.object;
      Node *index = node->as.index.index;
      node->as.index.object = NULL;
      node->as.index.index = NULL;
      freeNode(node);
      return newIndexSet(object, index, value, line);
    }

    if (node->type == NODE_FIELD_GET) {
      // `obj.field = value`: salvage the object, rebuild as a field set.
      Node *object = node->as.field.object;
      ObjString *field = node->as.field.field;
      node->as.field.object = NULL;
      freeNode(node);
      return newFieldSet(object, field, value, line);
    }

    // Invalid l-value, e.g. `1 + 2 = 3` or `(a) = 3`. Report but don't abort the
    // whole parse. We still free what we built to avoid a leak.
    errorAt(&parser.current, "Invalid assignment target.");
    freeNode(value);
    return node;
  }

  return node;
}

// `a or b or c` — left-associative chain. The short-circuit semantics live in
// the COMPILER (emit a jump that skips the right side when the left already
// decides the result); here we just build the tree.
static Node *logicOr(void) {
  Node *node = logicAnd();
  while (check(TOKEN_OR)) {
    int line = parser.current.line;
    advance();
    Node *right = logicAnd();
    node = newLogical(/*isAnd=*/false, node, right, line);
  }
  return node;
}

static Node *logicAnd(void) {
  Node *node = bitOr();
  while (check(TOKEN_AND)) {
    int line = parser.current.line;
    advance();
    Node *right = bitOr();
    node = newLogical(/*isAnd=*/true, node, right, line);
  }
  return node;
}

// Bitwise operators, in C's precedence order: | looser than ^ looser than &,
// all below equality. Each is a left-associative chain over integers.
static Node *bitOr(void) {
  Node *node = bitXor();
  while (check(TOKEN_PIPE)) {
    int line = parser.current.line;
    advance();
    node = newBinary(OP_NODE_BITOR, node, bitXor(), line);
  }
  return node;
}
static Node *bitXor(void) {
  Node *node = bitAnd();
  while (check(TOKEN_CARET)) {
    int line = parser.current.line;
    advance();
    node = newBinary(OP_NODE_BITXOR, node, bitAnd(), line);
  }
  return node;
}
static Node *bitAnd(void) {
  Node *node = equality();
  while (check(TOKEN_AMP)) {
    int line = parser.current.line;
    advance();
    node = newBinary(OP_NODE_BITAND, node, equality(), line);
  }
  return node;
}

static Node *equality(void) {
  Node *node = comparison();
  while (check(TOKEN_EQUAL_EQUAL) || check(TOKEN_BANG_EQUAL) || check(TOKEN_IS)) {
    int line = parser.current.line;
    if (check(TOKEN_IS)) {
      // `expr is TYPE` — the right side is a TYPE, not an expression.
      advance();
      node = newIs(node, parseType(), line);
      continue;
    }
    bool negate = check(TOKEN_BANG_EQUAL); // remember before consuming
    advance();
    Node *right = comparison();
    node = newBinary(OP_NODE_EQUAL, node, right, line);
    // Desugar `a != b` into `!(a == b)`.
    if (negate)
      node = newUnary(OP_NODE_NOT, node, line);
  }
  return node;
}

static Node *comparison(void) {
  Node *node = shift();
  while (check(TOKEN_LESS) || check(TOKEN_LESS_EQUAL) ||
         check(TOKEN_GREATER) || check(TOKEN_GREATER_EQUAL)) {
    int line = parser.current.line;
    TokenType op = parser.current.type;
    advance();
    Node *right = shift();
    // Build each form out of the two primitives `<` and `>` plus `!`:
    switch (op) {
    case TOKEN_LESS:
      node = newBinary(OP_NODE_LESS, node, right, line);
      break;
    case TOKEN_GREATER:
      node = newBinary(OP_NODE_GREATER, node, right, line);
      break;
    case TOKEN_LESS_EQUAL: // a <= b  ==>  !(a > b)
      node = newUnary(OP_NODE_NOT,
                      newBinary(OP_NODE_GREATER, node, right, line), line);
      break;
    case TOKEN_GREATER_EQUAL: // a >= b  ==>  !(a < b)
      node = newUnary(OP_NODE_NOT,
                      newBinary(OP_NODE_LESS, node, right, line), line);
      break;
    default:
      break; // unreachable
    }
  }
  return node;
}

// Bit shifts sit between comparison and additive (C precedence).
static Node *shift(void) {
  Node *node = term();
  while (check(TOKEN_LSHIFT) || check(TOKEN_RSHIFT)) {
    int line = parser.current.line;
    NodeOp op = check(TOKEN_LSHIFT) ? OP_NODE_SHL : OP_NODE_SHR;
    advance();
    node = newBinary(op, node, term(), line);
  }
  return node;
}

static Node *term(void) {
  Node *node = factor();
  while (check(TOKEN_PLUS) || check(TOKEN_MINUS)) {
    int line = parser.current.line;
    NodeOp op = match(TOKEN_PLUS) ? OP_NODE_ADD : (advance(), OP_NODE_SUB);
    Node *right = factor();
    node = newBinary(op, node, right, line);
  }
  return node;
}

static Node *factor(void) {
  Node *node = unary();
  while (check(TOKEN_STAR) || check(TOKEN_SLASH) || check(TOKEN_PERCENT)) {
    int line = parser.current.line;
    NodeOp op;
    if (match(TOKEN_STAR))
      op = OP_NODE_MUL;
    else if (match(TOKEN_SLASH))
      op = OP_NODE_DIV;
    else {
      advance(); // consume '%'
      op = OP_NODE_MOD;
    }
    Node *right = unary();
    node = newBinary(op, node, right, line);
  }
  return node;
}

static Node *call(void);

static Node *unary(void) {
  // Both prefix operators recurse into unary() (not primary) so stacked prefixes
  // like `--5` or `!!true` parse right-to-left as -(-5) / !(!true).
  if (match(TOKEN_MINUS)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_NEGATE, unary(), line);
  }
  if (match(TOKEN_BANG)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_NOT, unary(), line);
  }
  if (match(TOKEN_TILDE)) {
    int line = parser.previous.line;
    return newUnary(OP_NODE_BITNOT, unary(), line);
  }
  return call();
}

// Parse an argument list after a just-consumed '(' up to and including ')'.
// Arguments are expressions separated by commas; the count is capped at 255
// because the call/invoke operands are one byte. Returns the heap array (NULL if
// empty) and writes the count through `argCountOut`. Shared by calls and method
// invocations so the two stay identical.
static Node **parseArgList(int *argCountOut) {
  Node **args = NULL;
  int argCount = 0;
  int capacity = 0;
  if (!check(TOKEN_RPAREN)) {
    do {
      if (argCount == 255) {
        errorAt(&parser.current, "Cannot have more than 255 arguments.");
        break;
      }
      if (argCount + 1 > capacity) {
        capacity = capacity < 4 ? 4 : capacity * 2;
        args = realloc(args, sizeof(Node *) * capacity);
        if (args == NULL) {
          fprintf(stderr, "cnano: out of memory parsing arguments\n");
          exit(70);
        }
      }
      args[argCount++] = expression();
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RPAREN, "Expect ')' after arguments.");
  *argCountOut = argCount;
  return args;
}

// `callee(args)` — a plain call. The '(' has just been consumed.
static Node *finishCall(Node *callee) {
  int line = parser.previous.line; // the '('
  int argCount = 0;
  Node **args = parseArgList(&argCount);
  return newCall(callee, args, argCount, line);
}

// After a '.', a NAME is either a METHOD call (`a.m(args)`) or a FIELD access
// (`a.field`) — distinguished by whether a '(' follows. The '.' has just been
// consumed.
static Node *finishDot(Node *object) {
  int line = parser.previous.line; // the '.'
  consume(TOKEN_IDENTIFIER, "Expect a property or method name after '.'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  if (match(TOKEN_LPAREN)) {
    int argCount = 0;
    Node **args = parseArgList(&argCount);
    return newInvoke(object, name, args, argCount, line);
  }
  return newFieldGet(object, name, line); // assignment() may rewrite to a set
}

// `call -> primary ( "(" args ")" | "." NAME [ "(" args ")" ] | "[" expr "]" )*`
// — a primary followed by zero or more call / method / field / index suffixes.
// Looping lets `f()()`, `a.b().c`, and `m["k"].x` chain.
static Node *call(void) {
  Node *node = primary();
  for (;;) {
    if (match(TOKEN_LPAREN))
      node = finishCall(node);
    else if (match(TOKEN_DOT))
      node = finishDot(node);
    else if (match(TOKEN_LBRACKET)) {
      int line = parser.previous.line; // the '['
      Node *index = expression();
      consume(TOKEN_RBRACKET, "Expect ']' after index.");
      node = newIndexGet(node, index, line); // assignment() may rewrite to a set
    } else
      break;
  }
  return node;
}

// Decode a string literal's escape sequences (`\n \t \r \" \\ \$`, and `\X`
// passes X through) into a fresh interned ObjString.
static ObjString *decodeEscapes(const char *text, int len) {
  char *buf = malloc(len + 1);
  if (buf == NULL) {
    fprintf(stderr, "cnano: out of memory decoding a string\n");
    exit(70);
  }
  int o = 0;
  for (int i = 0; i < len; i++) {
    if (text[i] == '\\' && i + 1 < len) {
      char e = text[++i];
      buf[o++] = e == 'n'    ? '\n'
                 : e == 't'  ? '\t'
                 : e == 'r'  ? '\r'
                             : e; // \" \\ \$ and any other -> the char itself
    } else {
      buf[o++] = text[i];
    }
  }
  ObjString *s = copyString(buf, o);
  free(buf);
  return s;
}

// Parse one embedded `${ ... }` expression: lex+parse `text[0..len)` on a fresh
// temporary buffer, saving and restoring the outer lexer/parser cursor so the
// surrounding parse continues undisturbed.
static Node *parseEmbedded(const char *text, int len, int line) {
  char *buf = malloc(len + 1);
  if (buf == NULL) {
    fprintf(stderr, "cnano: out of memory in string interpolation\n");
    exit(70);
  }
  memcpy(buf, text, len);
  buf[len] = '\0';

  LexerState savedLexer = lexerSave();
  Token savedCur = parser.current, savedPrev = parser.previous;
  initLexer(buf);
  advance(); // prime `current` from the embedded buffer
  Node *e = expression();
  if (!check(TOKEN_EOF))
    errorAt(&parser.current, "Unexpected text after expression in '${ ... }'.");
  lexerRestore(savedLexer);
  parser.current = savedCur;
  parser.previous = savedPrev;
  free(buf);
  (void)line;
  return e;
}

// Desugar an interpolated string literal's inner text into a concatenation:
// each `${ expr }` becomes `str(expr)`, glued to the surrounding literal runs
// with `+`. Assumes the text contains at least one `${`.
static Node *interpolate(const char *text, int len, int line) {
  Node *result = NULL;
  int seg = 0;
  for (int i = 0; i < len;) {
    if (text[i] == '\\') { // an escape stays part of the literal run
      i += 2;
      continue;
    }
    if (text[i] == '$' && i + 1 < len && text[i + 1] == '{') {
      if (i > seg) { // flush the literal run before the hole (decoding escapes)
        Node *lit = newString(decodeEscapes(text + seg, i - seg), line);
        result = result ? newBinary(OP_NODE_ADD, result, lit, line) : lit;
      }
      int depth = 1, j = i + 2; // find the matching '}'
      while (j < len && depth > 0) {
        if (text[j] == '{')
          depth++;
        else if (text[j] == '}' && --depth == 0)
          break;
        j++;
      }
      Node *e = parseEmbedded(text + i + 2, j - (i + 2), line);
      Node **args = malloc(sizeof(Node *));
      if (args == NULL) { fprintf(stderr, "cnano: out of memory\n"); exit(70); }
      args[0] = e;
      // Wrap in str() so any value type converts to text.
      Node *call = newCall(newVarGet(copyString("str", 3), line), args, 1, line);
      result = result ? newBinary(OP_NODE_ADD, result, call, line) : call;
      i = j + 1;
      seg = i;
    } else {
      i++;
    }
  }
  if (len > seg) {
    Node *lit = newString(decodeEscapes(text + seg, len - seg), line);
    result = result ? newBinary(OP_NODE_ADD, result, lit, line) : lit;
  }
  return result ? result : newString(copyString("", 0), line);
}

// Decode a just-consumed NUMBER token into an int or float literal node. Handles
// the `0x`/`0b`/`0o` base prefixes and `_` digit separators the lexer permits:
// we copy the lexeme into a small buffer, drop the separators, and hand the clean
// digits to strtoll (with the right base) or strtod.
static Node *numberLiteral(void) {
  const char *s = parser.previous.start;
  int len = parser.previous.length;
  int line = parser.previous.line;

  char buf[64];
  int n = 0;
  bool isFloat = false;
  for (int i = 0; i < len && n < (int)sizeof(buf) - 1; i++) {
    if (s[i] == '_')
      continue; // strip digit separators
    if (s[i] == '.')
      isFloat = true; // (base-prefixed literals never contain '.')
    buf[n++] = s[i];
  }
  buf[n] = '\0';

  // Base prefixes. strtoll understands a leading "0x", but not "0b"/"0o", so we
  // skip those two prefixes ourselves and parse the remaining digits in base 2/8.
  if (n > 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
    return newInt((int64_t)strtoll(buf, NULL, 16), line);
  if (n > 2 && buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B'))
    return newInt((int64_t)strtoll(buf + 2, NULL, 2), line);
  if (n > 2 && buf[0] == '0' && (buf[1] == 'o' || buf[1] == 'O'))
    return newInt((int64_t)strtoll(buf + 2, NULL, 8), line);
  if (isFloat)
    return newFloat(strtod(buf, NULL), line);
  // Plain decimal — base 10 explicitly, so a leading-zero literal like 017 is the
  // integer 17, NOT C-style octal (a deliberate footgun avoided).
  return newInt((int64_t)strtoll(buf, NULL, 10), line);
}

static Node *primary(void) {
  if (match(TOKEN_FN)) {
    // An anonymous function expression (lambda): `fn(params) { … }` or the
    // `fn(params) => expr` shorthand. It compiles to the same closure a named
    // function does — it just isn't bound to a name. Synthesise "lambda" for
    // stack traces. (A *named* `fn` is parsed by funDeclaration at statement
    // level; `fn` reaching here is always anonymous.)
    return finishFunction(copyString("lambda", 6), parser.previous.line);
  }
  if (match(TOKEN_NUMBER))
    return numberLiteral();
  if (match(TOKEN_TRUE))
    return newBool(true, parser.previous.line);
  if (match(TOKEN_FALSE))
    return newBool(false, parser.previous.line);
  if (match(TOKEN_NIL))
    return newNil(parser.previous.line);
  if (match(TOKEN_STRING)) {
    // The inner text, without the surrounding quotes.
    const char *text = parser.previous.start + 1;
    int len = parser.previous.length - 2;
    int line = parser.previous.line;
    // If it contains an (unescaped) `${`, it's an interpolation; otherwise a
    // plain literal whose escapes we decode now.
    for (int i = 0; i + 1 < len; i++) {
      if (text[i] == '\\') {
        i++;
        continue;
      }
      if (text[i] == '$' && text[i + 1] == '{')
        return interpolate(text, len, line);
    }
    return newString(decodeEscapes(text, len), line);
  }
  if (match(TOKEN_IDENTIFIER)) {
    // Intern the variable's name so it can be used as a hash-table key. We build
    // a "read" node; assignment() rewrites it into an assign if a '=' follows.
    ObjString *name = copyString(parser.previous.start, parser.previous.length);
    return newVarGet(name, parser.previous.line);
  }
  if (match(TOKEN_LPAREN)) {
    Node *node = expression();
    consume(TOKEN_RPAREN, "Expect ')' after expression.");
    return node;
  }
  if (match(TOKEN_LBRACKET)) {
    // Array literal: `[e0, e1, ...]`. Same growable-list parse as an argument
    // list, but delimited by ']'.
    int line = parser.previous.line;
    Node **elements = NULL;
    int count = 0, capacity = 0;
    if (!check(TOKEN_RBRACKET)) {
      do {
        if (count == 255) {
          errorAt(&parser.current, "Cannot have more than 255 array elements.");
          break;
        }
        if (count + 1 > capacity) {
          capacity = capacity < 4 ? 4 : capacity * 2;
          elements = realloc(elements, sizeof(Node *) * capacity);
          if (elements == NULL) {
            fprintf(stderr, "cnano: out of memory parsing array literal\n");
            exit(70);
          }
        }
        elements[count++] = expression();
      } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RBRACKET, "Expect ']' after array elements.");
    return newArray(elements, count, line);
  }
  if (match(TOKEN_LBRACE)) {
    // Map literal: `{k0: v0, k1: v1, ...}`. Reached only in EXPRESSION position;
    // a `{` that begins a statement is parsed as a block (see statement()), so
    // there is no ambiguity — but a map-literal statement must be parenthesised
    // or used as a value (e.g. `let m = {..};`).
    int line = parser.previous.line;
    Node **keys = NULL;
    Node **values = NULL;
    int count = 0, capacity = 0;
    if (!check(TOKEN_RBRACE)) {
      do {
        if (count == 255) {
          errorAt(&parser.current, "Cannot have more than 255 map entries.");
          break;
        }
        if (count + 1 > capacity) {
          capacity = capacity < 4 ? 4 : capacity * 2;
          keys = realloc(keys, sizeof(Node *) * capacity);
          values = realloc(values, sizeof(Node *) * capacity);
          if (keys == NULL || values == NULL) {
            fprintf(stderr, "cnano: out of memory parsing map literal\n");
            exit(70);
          }
        }
        keys[count] = expression();
        consume(TOKEN_COLON, "Expect ':' between a map key and its value.");
        values[count] = expression();
        count++;
      } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RBRACE, "Expect '}' after map entries.");
    return newMap(keys, values, count, line);
  }
  errorAt(&parser.current, "Expect a value or '('.");
  // CRITICAL for error recovery: consume the offending token so the parser
  // always makes forward progress. Without this, a token that cannot start an
  // expression (e.g. a stray '}') is never advanced past, and the recovery loop
  // — which may early-return from synchronize() on a stale ';' — spins forever.
  // We only compile when parsing fully succeeds, so returning NULL here is safe.
  if (!check(TOKEN_EOF))
    advance();
  return NULL;
}

// --- statements ------------------------------------------------------------

static Node *statement(void);
static Node *declaration(void);

// After a syntax error we "panic": skip tokens until we reach a likely
// statement boundary, so parsing can resume and report further independent
// errors instead of one cascade. We stop just after a ';' (the end of the bad
// statement) or just before a token that clearly begins a new statement. This
// is classic panic-mode error recovery.
static void synchronize(void) {
  parser.panicMode = false;
  while (parser.current.type != TOKEN_EOF) {
    if (parser.previous.type == TOKEN_SEMICOLON)
      return; // just finished a statement; the next one can parse cleanly
    switch (parser.current.type) {
    case TOKEN_FN:
    case TOKEN_LET:
    case TOKEN_PRINT:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_FOR:
    case TOKEN_RETURN:
      return; // a keyword that starts a declaration/statement — resume here
    default:
      break;
    }
    advance();
  }
}

static Node *printStatement(void) {
  int line = parser.previous.line; // the 'print' keyword's line
  Node *value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after value.");
  return newPrint(value, line);
}

static Node *expressionStatement(void) {
  int line = parser.current.line;
  Node *expr = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
  // The wrapper records that this expression's value is to be DISCARDED — the
  // compiler will emit an OP_POP after it. Distinguishing this from `print`
  // here, at parse time, keeps the compiler simple.
  return newExprStmt(expr, line);
}

// `{ declaration* }` — parse statements into a fresh Program until the closing
// brace. The block OWNS that Program; the scope itself (which names are local) is
// purely a COMPILE-TIME concern handled later by the compiler, so the parser just
// records the nesting structure here.
static Node *block(void) {
  int line = parser.previous.line; // the '{'
  Program *body = malloc(sizeof(Program));
  if (body == NULL) {
    fprintf(stderr, "cnano: out of memory allocating block\n");
    exit(70);
  }
  initProgram(body);

  while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
    writeProgram(body, declaration());
    if (parser.panicMode)
      synchronize();
  }
  consume(TOKEN_RBRACE, "Expect '}' after block.");
  return newBlock(body, line);
}

static Node *varDeclaration(bool isConst); // used by forStatement's initialiser clause
static Type *parseType(void);      // used by forStatement's let-init clause

// `if (cond) thenStmt [else elseStmt]`. The parens are required (cnano follows
// the C family here). The branches are ordinary statements, so `if (c) { ... }`
// works because a block is a statement.
static Node *ifStatement(void) {
  int line = parser.previous.line; // the 'if'
  consume(TOKEN_LPAREN, "Expect '(' after 'if'.");
  Node *condition = expression();
  consume(TOKEN_RPAREN, "Expect ')' after condition.");
  Node *then = statement();
  Node *otherwise = NULL;
  if (match(TOKEN_ELSE))
    otherwise = statement(); // a bare `else` binds to the nearest `if`
  return newIf(condition, then, otherwise, line);
}

static Node *whileStatement(void) {
  int line = parser.previous.line; // the 'while'
  consume(TOKEN_LPAREN, "Expect '(' after 'while'.");
  Node *condition = expression();
  consume(TOKEN_RPAREN, "Expect ')' after condition.");
  Node *body = statement();
  return newWhile(condition, body, line);
}

// `do { body } while (cond);` — the body runs once before the first test.
static Node *doWhileStatement(void) {
  int line = parser.previous.line; // the 'do'
  Node *body = statement();
  consume(TOKEN_WHILE, "Expect 'while' after a 'do' body.");
  consume(TOKEN_LPAREN, "Expect '(' after 'while'.");
  Node *condition = expression();
  consume(TOKEN_RPAREN, "Expect ')' after condition.");
  consume(TOKEN_SEMICOLON, "Expect ';' after a do/while loop.");
  return newDoWhile(condition, body, line);
}

// `for (init; cond; update) body` — implemented entirely as SYNTACTIC SUGAR over
// constructs we already have. There is no for-loop node and no for-loop opcode;
// the parser rewrites
//
//     for (init; cond; update) body
//
// into the equivalent
//
//     { init; while (cond) { body; update; } }
//
// The outer block scopes the loop variable; the while drives the iteration; the
// update is appended to the body. A missing condition becomes `true` (infinite
// loop). This is the same trick step 1 used for `!=`/`<=`: keep the core tiny,
// express conveniences by lowering them to it. Desugaring a whole statement form
// (not just an operator) is a powerful demonstration of the idea.
// Allocate a Program (a growable node list) on the heap — the container a block
// node owns. Small helper so the desugars below read cleanly.
static Program *makeProgram(void) {
  Program *p = malloc(sizeof(Program));
  if (p == NULL) {
    fprintf(stderr, "cnano: out of memory desugaring a loop\n");
    exit(70);
  }
  initProgram(p);
  return p;
}

// Desugar `for (let VAR in COLL) BODY` into an index loop over a hidden sequence:
//
//   { let $seq = $for_iter(COLL); let $i = 0;
//     while ($i < $seq.len()) { let VAR = $seq[$i]; BODY; $i = $i + 1; } }
//
// $for_iter (a hidden builtin) yields the array to walk — the array itself, or a
// map's keys — so this one shape covers both, even when COLL's type is dynamic.
// The synthesised names use a leading '$' (unlexable), so they cannot collide
// with user variables; block scoping makes nested for-in loops independent.
static Node *desugarForIn(ObjString *var, Node *coll, Node *body, int line) {
  ObjString *seq = copyString("$seq", 4);
  ObjString *idx = copyString("$i", 2);

  // let $seq = $for_iter(COLL);
  Node **iterArgs = malloc(sizeof(Node *));
  if (iterArgs == NULL) {
    fprintf(stderr, "cnano: out of memory desugaring for-in\n");
    exit(70);
  }
  iterArgs[0] = coll;
  Node *iterCall =
      newCall(newVarGet(copyString("$for_iter", 9), line), iterArgs, 1, line);
  Node *declSeq = newVarDecl(seq, iterCall, typeAny(), line);
  Node *declIdx = newVarDecl(idx, newInt(0, line), typeAny(), line);

  // while ($i < $seq.len()) { ... }
  Node *lenCall = newInvoke(newVarGet(seq, line), copyString("len", 3), NULL, 0, line);
  Node *cond = newBinary(OP_NODE_LESS, newVarGet(idx, line), lenCall, line);

  Node *element = newIndexGet(newVarGet(seq, line), newVarGet(idx, line), line);
  Node *declVar = newVarDecl(var, element, typeAny(), line);
  Node *incr = newAssign(
      idx, newBinary(OP_NODE_ADD, newVarGet(idx, line), newInt(1, line), line),
      line);
  Program *loopBody = makeProgram();
  writeProgram(loopBody, declVar);
  writeProgram(loopBody, body);
  Node *whileNode = newWhile(cond, newBlock(loopBody, line), line);
  whileNode->as.whileStmt.increment = incr; // `$i = $i + 1`, so `continue` runs it

  Program *outer = makeProgram();
  writeProgram(outer, declSeq);
  writeProgram(outer, declIdx);
  writeProgram(outer, whileNode);
  return newBlock(outer, line);
}

// Desugar `for (let NAME in LO..HI) BODY` into a counting loop:
//   { let NAME = LO; let $end = HI; while (NAME < $end) { BODY; NAME = NAME + 1; } }
// HI is evaluated ONCE (into $end), and the step rides the while's increment so
// `continue` runs it. The end is exclusive: 0..n yields 0,1,...,n-1.
static Node *desugarRange(ObjString *name, Node *lo, Node *hi, Node *body,
                          int line) {
  ObjString *end = copyString("$end", 4);
  Node *declVar = newVarDecl(name, lo, typeAny(), line);
  Node *declEnd = newVarDecl(end, hi, typeAny(), line);
  Node *cond =
      newBinary(OP_NODE_LESS, newVarGet(name, line), newVarGet(end, line), line);
  Node *incr = newAssign(
      name, newBinary(OP_NODE_ADD, newVarGet(name, line), newInt(1, line), line),
      line);
  Node *loop = newWhile(cond, body, line);
  loop->as.whileStmt.increment = incr;

  Program *outer = makeProgram();
  writeProgram(outer, declVar);
  writeProgram(outer, declEnd);
  writeProgram(outer, loop);
  return newBlock(outer, line);
}

static Node *forStatement(void) {
  int line = parser.previous.line; // the 'for'
  consume(TOKEN_LPAREN, "Expect '(' after 'for'.");

  // --- initialiser clause (also the for-in fork) ---
  Node *initializer = NULL;
  if (match(TOKEN_SEMICOLON)) {
    initializer = NULL; // no initialiser
  } else if (match(TOKEN_LET)) {
    // After `let NAME`, one token of lookahead distinguishes the two loops:
    //   `in`  -> for-in;   `:` or `=` -> a C-style let-initialiser.
    consume(TOKEN_IDENTIFIER, "Expect a variable name after 'let'.");
    ObjString *name = copyString(parser.previous.start, parser.previous.length);
    if (match(TOKEN_IN)) {
      Node *coll = expression();
      if (match(TOKEN_DOTDOT)) {
        // A numeric range `LO..HI` — count instead of iterating a collection.
        Node *hi = expression();
        consume(TOKEN_RPAREN, "Expect ')' after the range.");
        Node *body = statement();
        return desugarRange(name, coll, hi, body, line);
      }
      consume(TOKEN_RPAREN, "Expect ')' after the for-in collection.");
      Node *body = statement();
      return desugarForIn(name, coll, body, line);
    }
    // C-style let-initialiser: finish parsing `[: TYPE] = EXPR ;` by hand (we have
    // already consumed `let NAME`, so we can't call varDeclaration here).
    Type *declaredType = match(TOKEN_COLON) ? parseType() : typeAny();
    consume(TOKEN_EQUAL, "Expect '=' after the loop variable name.");
    Node *init = expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after the loop initialiser.");
    initializer = newVarDecl(name, init, declaredType, line);
  } else {
    initializer = expressionStatement(); // consumes its own trailing ';'
  }

  // --- condition clause (optional) ---
  Node *condition = NULL;
  if (!check(TOKEN_SEMICOLON))
    condition = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");

  // --- update clause (optional) ---
  Node *update = NULL;
  if (!check(TOKEN_RPAREN))
    update = expression();
  consume(TOKEN_RPAREN, "Expect ')' after for clauses.");

  Node *body = statement();

  // --- desugar into a while with an explicit increment ---
  // The update runs after the body each iteration, but as the while's `increment`
  // (not appended to the body) so `continue` runs it instead of skipping it.
  if (condition == NULL)
    condition = newBool(true, line); // a missing condition means "loop forever"
  Node *loop = newWhile(condition, body, line);
  loop->as.whileStmt.increment = update; // may be NULL
  body = loop;

  // If there is an initialiser, wrap the whole thing in a block so the loop
  // variable's scope is the loop:  { init; while (...) {...} }
  if (initializer != NULL) {
    Program *outer = malloc(sizeof(Program));
    if (outer == NULL) {
      fprintf(stderr, "cnano: out of memory desugaring for-loop\n");
      exit(70);
    }
    initProgram(outer);
    writeProgram(outer, initializer);
    writeProgram(outer, body);
    body = newBlock(outer, line);
  }

  return body;
}

// `return EXPR? ;` — return from the enclosing function. A bare `return;`
// returns nil (handled by the compiler). Returning from the top-level script is
// rejected at COMPILE time, not here, since the parser doesn't track that.
static Node *returnStatement(void) {
  int line = parser.previous.line; // the 'return'
  Node *value = NULL;
  if (!check(TOKEN_SEMICOLON))
    value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
  return newReturn(value, line);
}

// `throw EXPR;` — raise a value to the nearest enclosing catch.
static Node *throwStatement(void) {
  int line = parser.previous.line; // the 'throw'
  Node *value = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after the thrown value.");
  return newThrow(value, line);
}

// `try { body } catch (name) { handler }` — run body, and on a throw bind the
// thrown value to `name` and run handler.
static Node *tryStatement(void) {
  int line = parser.previous.line; // the 'try'
  consume(TOKEN_LBRACE, "Expect '{' after 'try'.");
  Node *body = block();
  consume(TOKEN_CATCH, "Expect 'catch' after the try block.");
  consume(TOKEN_LPAREN, "Expect '(' after 'catch'.");
  consume(TOKEN_IDENTIFIER, "Expect a catch variable name.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  consume(TOKEN_RPAREN, "Expect ')' after the catch variable.");
  consume(TOKEN_LBRACE, "Expect '{' before the catch block.");
  Node *handler = block();
  return newTry(body, name, handler, line);
}

// `match (SUBJECT) { p1 => s1; p2 => s2; _ => sd; }` — value dispatch. Desugars
// to evaluate SUBJECT once and run an if/else-if chain comparing it (`==`) to each
// pattern, with `_` as the default. Patterns are ordinary expressions (usually
// literals). Arms are statements (often blocks); no separators are needed.
static Node *matchStatement(void) {
  int line = parser.previous.line; // 'match'
  consume(TOKEN_LPAREN, "Expect '(' after 'match'.");
  Node *subject = expression();
  consume(TOKEN_RPAREN, "Expect ')' after the match subject.");
  consume(TOKEN_LBRACE, "Expect '{' to begin the match arms.");
  if (subject == NULL)
    return NULL; // a parse error in the subject; resynchronise rather than deref

  // If the subject is already a plain variable, we test that variable directly.
  // This matters for `is TYPE =>` arms: the narrowing logic keys off the *name*
  // of the variable being tested, so reusing the subject's own name lets the
  // body of a type arm see the narrowed type. For any other subject expression
  // we bind it to a hidden `$m` temp first (so it is evaluated exactly once).
  bool subjectIsVar = subject->type == NODE_VAR_GET;
  ObjString *subjName = subjectIsVar ? subject->as.name : copyString("$m", 2);

  Node *patterns[256]; // value pattern (NULL for an `is TYPE` arm)
  Type *types[256];    // type pattern (NULL for a value arm)
  Node *bodies[256];
  int count = 0;
  Node *defaultBody = NULL;
  while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
    bool isDefault = check(TOKEN_IDENTIFIER) && parser.current.length == 1 &&
                     parser.current.start[0] == '_';
    Node *pat = NULL;
    Type *ty = NULL;
    if (isDefault)
      advance(); // consume '_'
    else if (check(TOKEN_IS)) {
      // `is TYPE => …` — a type-pattern arm. The right of `is` is a TYPE.
      advance();
      ty = parseType();
    } else
      pat = expression();
    consume(TOKEN_FAT_ARROW, "Expect '=>' after a match pattern.");
    Node *body = statement();
    if (isDefault) {
      if (defaultBody != NULL)
        errorAt(&parser.previous, "A match can have only one '_' arm.");
      defaultBody = body;
    } else if (count < 256) {
      patterns[count] = pat;
      types[count] = ty;
      bodies[count] = body;
      count++;
    }
  }
  consume(TOKEN_RBRACE, "Expect '}' after the match arms.");

  // { [let $m = SUBJECT;] if (subj == p0) b0 else if (subj is T1) b1 ... else default }
  Node *chain = defaultBody; // the innermost else (may be NULL)
  for (int i = count - 1; i >= 0; i--) {
    Node *cond;
    if (types[i] != NULL)
      cond = newIs(newVarGet(subjName, line), types[i], line);
    else
      cond = newBinary(OP_NODE_EQUAL, newVarGet(subjName, line), patterns[i],
                       line);
    chain = newIf(cond, bodies[i], chain, line);
  }

  // Build the lowered body the backends will compile (unchanged from before).
  Node *body;
  if (subjectIsVar) {
    // We only borrowed the subject's name; free the now-orphaned subject node.
    freeNode(subject);
    body = chain != NULL ? chain : newBlock(makeProgram(), line);
  } else {
    Program *outer = makeProgram();
    writeProgram(outer, newVarDecl(subjName, subject, typeAny(), line));
    if (chain != NULL)
      writeProgram(outer, chain);
    body = newBlock(outer, line);
  }

  // Describe each VALUE arm for the exhaustiveness check (purely syntactic — the
  // checker confirms `enumName` is really an enum and which members exist).
  MatchArm *arms = count > 0 ? malloc(sizeof(MatchArm) * count) : NULL;
  if (count > 0 && arms == NULL) {
    fprintf(stderr, "cnano: out of memory describing match arms\n");
    exit(70);
  }
  for (int i = 0; i < count; i++) {
    Node *p = patterns[i];
    if (types[i] != NULL) {
      arms[i].kind = MATCH_ARM_TYPE; // an `is T` arm — for union exhaustiveness
      arms[i].type = types[i];
    } else if (p != NULL && p->type == NODE_BOOL) {
      arms[i].kind = MATCH_ARM_BOOL;
      arms[i].boolVal = p->as.boolValue;
    } else if (p != NULL && p->type == NODE_FIELD_GET &&
               p->as.field.object->type == NODE_VAR_GET) {
      arms[i].kind = MATCH_ARM_ENUM; // looks like `Enum.Member`
      arms[i].enumName = p->as.field.object->as.name;
      arms[i].member = p->as.field.field;
    } else {
      arms[i].kind = MATCH_ARM_OTHER; // a value arm we can't reason about
    }
  }
  // Exhaustiveness is only attempted when the subject is a plain variable (so we
  // can look up its type); otherwise subjectName stays NULL.
  return newMatch(body, subjectIsVar ? subjName : NULL, arms, count,
                  defaultBody != NULL, line);
}

static Node *statement(void) {
  if (match(TOKEN_MATCH))
    return matchStatement();
  if (match(TOKEN_PRINT))
    return printStatement();
  if (match(TOKEN_IF))
    return ifStatement();
  if (match(TOKEN_WHILE))
    return whileStatement();
  if (match(TOKEN_DO))
    return doWhileStatement();
  if (match(TOKEN_FOR))
    return forStatement();
  if (match(TOKEN_RETURN))
    return returnStatement();
  if (match(TOKEN_THROW))
    return throwStatement();
  if (match(TOKEN_YIELD)) {
    int line = parser.previous.line;
    parser.fnSawYield = true; // marks the enclosing function as a generator
    Node *value = expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after a yielded value.");
    return newYield(value, line);
  }
  if (match(TOKEN_TRY))
    return tryStatement();
  if (match(TOKEN_BREAK)) {
    int line = parser.previous.line;
    consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");
    return newBreak(line);
  }
  if (match(TOKEN_CONTINUE)) {
    int line = parser.previous.line;
    consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
    return newContinue(line);
  }
  if (match(TOKEN_LBRACE))
    return block();
  return expressionStatement();
}

// Parse a TYPE expression and return its structured Type*. A type is one of:
//   * a primitive name: int / bool / str / any (identifiers) or the `nil` keyword
//   * an array type:  [ TYPE ]
//   * a map type:     { TYPE : TYPE }
// Types nest, so this recurses: `[[int]]` and `{str: [int]}` both parse. Type
// names are NOT reserved keywords — they are recognised only here, in annotation
// position, so `int` etc. remain usable as ordinary variable names elsewhere.
static Type *parseTypeBase(void) {
  if (match(TOKEN_LBRACKET)) { // [ ELEMENT ]
    Type *element = parseType();
    consume(TOKEN_RBRACKET, "Expect ']' to close an array type.");
    return typeArray(element);
  }
  if (match(TOKEN_LBRACE)) { // { KEY : VALUE }
    Type *key = parseType();
    consume(TOKEN_COLON, "Expect ':' between a map's key and value types.");
    Type *value = parseType();
    consume(TOKEN_RBRACE, "Expect '}' to close a map type.");
    return typeMap(key, value);
  }
  if (match(TOKEN_NIL)) // `nil` is a keyword (the literal) but also a type name
    return typeNil();
  if (match(TOKEN_IDENTIFIER)) {
    int len = parser.previous.length;
    const char *s = parser.previous.start;
    if (len == 3 && memcmp(s, "int", 3) == 0)
      return typeInt();
    if (len == 5 && memcmp(s, "float", 5) == 0)
      return typeFloat();
    if (len == 4 && memcmp(s, "bool", 4) == 0)
      return typeBool();
    if (len == 3 && memcmp(s, "str", 3) == 0)
      return typeStr();
    if (len == 3 && memcmp(s, "any", 3) == 0)
      return typeAny();
    // Any other identifier names a (user-defined) struct type. We can't resolve
    // it here — the parser doesn't know the declarations — so we record it as an
    // unresolved reference; the type checker matches it to a `struct` by name.
    return typeStructRef(copyString(s, len));
  }
  errorAt(&parser.current, "Expect a type after ':'.");
  return typeAny();
}

// A base type with zero or more `?` nullable markers (`int?`, `[int]?`).
static Type *parseNullable(void) {
  Type *t = parseTypeBase();
  while (match(TOKEN_QUESTION))
    t = typeNullable(t);
  return t;
}

// A type is a `|`-separated union of nullable base types (`int | str`,
// `Circle | Square | nil`). A single alternative is just that type.
static Type *parseType(void) {
  Type *t = parseNullable();
  while (match(TOKEN_PIPE))
    t = typeUnite(t, parseNullable());
  return t;
}

// `let NAME [: TYPE] = EXPR ;` — declare and initialise a variable. The type
// annotation is optional; omitted means TY_ANY (stay dynamic). We require an
// initialiser for simplicity, sidestepping the "uninitialised variable" question.
static Node *varDeclaration(bool isConst) {
  int line = parser.previous.line; // the 'let' / 'const' keyword's line
  consume(TOKEN_IDENTIFIER,
          isConst ? "Expect a name after 'const'." : "Expect variable name after 'let'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  Type *declaredType = match(TOKEN_COLON) ? parseType() : typeAny();
  consume(TOKEN_EQUAL, "Expect '=' after the name.");
  Node *initializer = expression();
  consume(TOKEN_SEMICOLON, "Expect ';' after the declaration.");
  Node *node = newVarDecl(name, initializer, declaredType, line);
  node->as.var.isConst = isConst;
  return node;
}

// `fn NAME ( params ) { body }`. We parse the parameter names into a heap array,
// then the body as a block. The compiler turns this into an ObjFunction. Like
// `let`, a function declaration BINDS a name (so functions can be called, and
// can recurse / be mutually recursive among globals).
// Parse the part of a function after its name (or after `fn` for a lambda):
//   "(" params ")" [":" TYPE] ( "{" block "}" | "=>" expression )
// `name` is the function's name — a synthetic "lambda" for anonymous functions.
// The `=> expr` shorthand desugars to a body of `{ return expr; }`.
static Node *finishFunction(ObjString *name, int line) {
  // Optional generic type parameters: `fn id<T, U>(…)`. The `<` here is
  // unambiguous — it follows a function name in declaration position, never an
  // expression, so it cannot be the comparison operator.
  ObjString **typeParams = NULL;
  int typeParamCount = 0;
  if (match(TOKEN_LESS)) {
    do {
      consume(TOKEN_IDENTIFIER, "Expect type-parameter name.");
      typeParams = realloc(typeParams, sizeof(ObjString *) * (typeParamCount + 1));
      if (typeParams == NULL) {
        fprintf(stderr, "cnano: out of memory parsing type parameters\n");
        exit(70);
      }
      typeParams[typeParamCount++] =
          copyString(parser.previous.start, parser.previous.length);
    } while (match(TOKEN_COMMA));
    consume(TOKEN_GREATER, "Expect '>' after type parameters.");
  }

  consume(TOKEN_LPAREN, "Expect '(' after a function's parameter list.");
  ObjString **params = NULL;
  Type **paramTypes = NULL;
  int paramCount = 0;
  int capacity = 0;
  if (!check(TOKEN_RPAREN)) {
    do {
      if (paramCount == 255) {
        errorAt(&parser.current, "Cannot have more than 255 parameters.");
        break;
      }
      consume(TOKEN_IDENTIFIER, "Expect parameter name.");
      if (paramCount + 1 > capacity) {
        capacity = capacity < 4 ? 4 : capacity * 2;
        params = realloc(params, sizeof(ObjString *) * capacity);
        paramTypes = realloc(paramTypes, sizeof(Type *) * capacity);
        if (params == NULL || paramTypes == NULL) {
          fprintf(stderr, "cnano: out of memory parsing parameters\n");
          exit(70);
        }
      }
      params[paramCount] =
          copyString(parser.previous.start, parser.previous.length);
      // Optional `: TYPE` per parameter; default any.
      paramTypes[paramCount] = match(TOKEN_COLON) ? parseType() : typeAny();
      paramCount++;
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RPAREN, "Expect ')' after parameters.");

  // Optional return-type annotation: `fn f(...) : TYPE { ... }`. When omitted the
  // checker INFERS the return type from the body, so record that it was absent.
  bool returnAnnotated = match(TOKEN_COLON);
  Type *returnType = returnAnnotated ? parseType() : typeAny();

  // Track whether THIS function's body contains a `yield` (save/restore so a
  // nested function doesn't leak its flag to the enclosing one).
  bool savedSawYield = parser.fnSawYield;
  parser.fnSawYield = false;

  Program *body;
  if (match(TOKEN_FAT_ARROW)) {
    // `=> expr` shorthand: a single-expression body, desugared to `return expr;`.
    int aline = parser.previous.line;
    Node *value = expression();
    body = makeProgram();
    writeProgram(body, newReturn(value, aline));
  } else {
    consume(TOKEN_LBRACE, "Expect '{' or '=>' before a function body.");
    Node *bodyBlock = block(); // parses up to and including the closing '}'
    // block() returns a NODE_BLOCK owning a Program; unwrap it for the fun node.
    body = bodyBlock->as.block;
    bodyBlock->as.block = NULL; // detach so freeing the wrapper won't free body
    freeNode(bodyBlock);
  }

  Node *fn = newFun(name, params, paramTypes, paramCount, returnType, body, line);
  fn->as.fun.typeParams = typeParams;
  fn->as.fun.typeParamCount = typeParamCount;
  fn->as.fun.returnAnnotated = returnAnnotated;
  fn->as.fun.isGenerator = parser.fnSawYield;
  parser.fnSawYield = savedSawYield;
  return fn;
}

static Node *funDeclaration(void) {
  int line = parser.previous.line; // the 'fn'
  consume(TOKEN_IDENTIFIER, "Expect function name after 'fn'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  return finishFunction(name, line);
}

// `struct NAME { field [: TYPE] (, field [: TYPE])* }` — a record-type
// declaration. Binds NAME to a callable struct object; `NAME(args)` constructs an
// instance with the fields in declaration order.
static Node *structDeclaration(void) {
  int line = parser.previous.line; // the 'struct'
  consume(TOKEN_IDENTIFIER, "Expect a struct name after 'struct'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  consume(TOKEN_LBRACE, "Expect '{' after the struct name.");

  ObjString **fieldNames = NULL;
  Type **fieldTypes = NULL;
  int count = 0, capacity = 0;
  Node **methods = NULL;
  int methodCount = 0, methodCap = 0;
  while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
    if (match(TOKEN_FN)) {
      // A method: an ordinary function declaration whose body may use `self`.
      Node *m = funDeclaration();
      if (methodCount + 1 > methodCap) {
        methodCap = methodCap < 4 ? 4 : methodCap * 2;
        methods = realloc(methods, sizeof(Node *) * methodCap);
        if (methods == NULL) {
          fprintf(stderr, "cnano: out of memory parsing struct methods\n");
          exit(70);
        }
      }
      methods[methodCount++] = m;
      continue;
    }
    consume(TOKEN_IDENTIFIER, "Expect a field name.");
    ObjString *fname = copyString(parser.previous.start, parser.previous.length);
    Type *ftype = match(TOKEN_COLON) ? parseType() : typeAny();
    if (count + 1 > capacity) {
      capacity = capacity < 4 ? 4 : capacity * 2;
      fieldNames = realloc(fieldNames, sizeof(ObjString *) * capacity);
      fieldTypes = realloc(fieldTypes, sizeof(Type *) * capacity);
      if (fieldNames == NULL || fieldTypes == NULL) {
        fprintf(stderr, "cnano: out of memory parsing struct fields\n");
        exit(70);
      }
    }
    fieldNames[count] = fname;
    fieldTypes[count] = ftype;
    count++;
    match(TOKEN_COMMA); // a separating/trailing comma between fields is optional
  }
  consume(TOKEN_RBRACE, "Expect '}' after the struct body.");
  Node *node = newStructDecl(name, fieldNames, fieldTypes, count, line);
  node->as.structDecl.methods = methods;
  node->as.structDecl.methodCount = methodCount;
  return node;
}

// One level above statement(): a declaration is a `struct`, a `fn`, a `let`, or
// any statement. This is the natural synchronisation point for errors.
// `import "path";` — a top-level statement that pulls another file's
// declarations into this program. The path is a plain string literal (no
// interpolation): a compile-time constant, resolved by the module loader before
// type-checking, never reaching the VM. We decode the literal's escapes so a
// path may contain e.g. a space written as-is.
static Node *importDeclaration(void) {
  int line = parser.previous.line; // 'import'
  consume(TOKEN_STRING, "Expect a \"path\" string after 'import'.");
  const char *text = parser.previous.start + 1; // strip the quotes
  int len = parser.previous.length - 2;
  // Reject interpolation in a path: it must be a compile-time constant.
  for (int i = 0; i + 1 < len; i++) {
    if (text[i] == '\\') { i++; continue; }
    if (text[i] == '$' && text[i + 1] == '{') {
      errorAt(&parser.previous, "An import path must be a plain string literal.");
      break;
    }
  }
  ObjString *path = decodeEscapes(text, len);
  consume(TOKEN_SEMICOLON, "Expect ';' after an import path.");
  return newImport(path, line);
}

// `enum Name { A, B, C }` — a comma-separated list of member names (a trailing
// comma is allowed). Each member becomes a singleton accessible as `Name.A`.
static Node *enumDeclaration(void) {
  int line = parser.previous.line; // the 'enum'
  consume(TOKEN_IDENTIFIER, "Expect enum name after 'enum'.");
  ObjString *name = copyString(parser.previous.start, parser.previous.length);
  consume(TOKEN_LBRACE, "Expect '{' after enum name.");

  ObjString **members = NULL;
  int count = 0, capacity = 0;
  if (!check(TOKEN_RBRACE)) {
    do {
      if (check(TOKEN_RBRACE)) // tolerate a trailing comma before '}'
        break;
      consume(TOKEN_IDENTIFIER, "Expect a member name.");
      if (count + 1 > capacity) {
        capacity = capacity < 4 ? 4 : capacity * 2;
        members = realloc(members, sizeof(ObjString *) * capacity);
        if (members == NULL) {
          fprintf(stderr, "cnano: out of memory parsing enum members\n");
          exit(70);
        }
      }
      members[count++] =
          copyString(parser.previous.start, parser.previous.length);
    } while (match(TOKEN_COMMA));
  }
  consume(TOKEN_RBRACE, "Expect '}' after enum members.");
  if (count == 0)
    errorAt(&parser.previous, "An enum must have at least one member.");
  return newEnumDecl(name, members, count, line);
}

static Node *declaration(void) {
  if (match(TOKEN_IMPORT))
    return importDeclaration();
  if (match(TOKEN_STRUCT))
    return structDeclaration();
  if (match(TOKEN_ENUM))
    return enumDeclaration();
  if (match(TOKEN_FN))
    return funDeclaration();
  if (match(TOKEN_LET))
    return varDeclaration(/*isConst=*/false);
  if (match(TOKEN_CONST))
    return varDeclaration(/*isConst=*/true);
  return statement();
}

bool parse(const char *source, Program *out, int lineBase) {
  initLexer(source);
  lexerSetLine(lineBase + 1); // this file's lines occupy [lineBase+1, lineBase+SPAN)
  initProgram(out);
  parser.hadError = false;
  parser.panicMode = false;
  advance(); // prime `current` with the first token

  // The program loop: parse statements until end of file. On a syntax error we
  // synchronize and keep going, collecting as many independent errors as we can
  // in a single run — much friendlier than stopping at the first.
  while (!check(TOKEN_EOF)) {
    Node *decl = declaration();
    writeProgram(out, decl);
    if (parser.panicMode)
      synchronize();
  }

  return !parser.hadError;
}

// lexer.h — turns raw source text into a stream of tokens.
//
// The lexer (a.k.a. scanner or tokenizer) is the compiler's first stage. It
// reads characters and groups them into the smallest meaningful units —
// "tokens" — so the parser can think in terms of NUMBER, PLUS, LPAREN instead
// of individual characters. This separation is one of the oldest and most
// useful ideas in compiler construction.
#ifndef CNANO_LEXER_H
#define CNANO_LEXER_H

typedef enum {
  // Single-character punctuation
  TOKEN_PLUS,
  TOKEN_MINUS,
  TOKEN_STAR,
  TOKEN_SLASH,
  TOKEN_PERCENT,   // % — integer remainder (modulo)
  // Compound assignment: x += e desugars to x = x + e (parser, no new opcodes).
  TOKEN_PLUS_EQUAL,
  TOKEN_MINUS_EQUAL,
  TOKEN_STAR_EQUAL,
  TOKEN_SLASH_EQUAL,
  TOKEN_PERCENT_EQUAL,
  TOKEN_AMP_EQUAL,    // &=
  TOKEN_PIPE_EQUAL,   // |=
  TOKEN_CARET_EQUAL,  // ^=
  TOKEN_LSHIFT_EQUAL, // <<=
  TOKEN_RSHIFT_EQUAL, // >>=
  // Bitwise operators (integers only).
  TOKEN_AMP,       // &
  TOKEN_PIPE,      // |
  TOKEN_CARET,     // ^
  TOKEN_TILDE,     // ~  (unary bitwise NOT)
  TOKEN_LSHIFT,    // <<
  TOKEN_RSHIFT,    // >>
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  TOKEN_LBRACE,    // { — opens a block scope (or a map type/literal)
  TOKEN_RBRACE,    // } — closes a block scope
  TOKEN_LBRACKET,  // [ — opens an array type/literal or an index
  TOKEN_RBRACKET,  // ] — closes one
  TOKEN_COMMA,     // , — separates parameters and arguments
  TOKEN_DOT,       // . — method call: receiver.method(args)
  TOKEN_DOTDOT,    // .. — an integer range, as in `for (let i in 0..n)`
  TOKEN_COLON,     // : — introduces a type annotation
  TOKEN_QUESTION,  // ? — marks a nullable type, as in `int?`
  TOKEN_SEMICOLON, // ; — terminates a statement
  // One- or two-character operators. Some of these share a first character
  // (`!` vs `!=`, `<` vs `<=`), so the lexer must peek at the SECOND character
  // to decide which token it is — the first taste of multi-character lexing.
  TOKEN_BANG,          // !
  TOKEN_BANG_EQUAL,    // !=
  TOKEN_EQUAL,         // =  (assignment)
  TOKEN_EQUAL_EQUAL,   // ==
  TOKEN_LESS,          // <
  TOKEN_LESS_EQUAL,    // <=
  TOKEN_GREATER,       // >
  TOKEN_GREATER_EQUAL, // >=
  // Literals
  TOKEN_NUMBER,
  TOKEN_STRING,     // a "double-quoted" string literal
  TOKEN_IDENTIFIER, // a variable name like `x` or `count`
  // Keywords. These are spelled like identifiers but have reserved meaning.
  TOKEN_TRUE,
  TOKEN_FALSE,
  TOKEN_NIL,
  TOKEN_PRINT,
  TOKEN_LET,
  TOKEN_CONST,     // const — an immutable binding
  TOKEN_IF,
  TOKEN_ELSE,
  TOKEN_WHILE,
  TOKEN_DO,        // do — begins a `do { } while (c);` loop
  TOKEN_FOR,
  TOKEN_IN,        // in — used by for-in iteration: `for (let x in coll)`
  TOKEN_IS,        // is — runtime type test: `x is int` (also narrows unions)
  TOKEN_AND,
  TOKEN_OR,
  TOKEN_FN,
  TOKEN_RETURN,
  TOKEN_STRUCT,    // struct — declares a user-defined record type
  TOKEN_ENUM,      // enum — declares a set of named constant members
  TOKEN_TRY,       // try — begins an exception-guarded block
  TOKEN_CATCH,     // catch — handles a thrown value
  TOKEN_THROW,     // throw — raises a value to the nearest catch
  TOKEN_BREAK,     // break — exit the innermost loop
  TOKEN_CONTINUE,  // continue — skip to the innermost loop's next iteration
  TOKEN_MATCH,     // match — value-dispatch over a subject
  TOKEN_IMPORT,    // import — pull in another source file's declarations
  TOKEN_FAT_ARROW, // => — separates a match pattern from its body
  // Bookkeeping
  TOKEN_ERROR, // lexing failed; `start`/`length` point at a human message
  TOKEN_EOF,   // end of input — lets the parser stop without special-casing
} TokenType;

// A token does NOT copy the text it represents. Instead it stores a pointer
// into the original source plus a length (a "string slice"). This is a common
// performance technique: zero allocation per token, and we still keep the whole
// source alive for error reporting. The trade-off is that the source buffer
// must outlive the tokens — which it does here.
typedef struct {
  TokenType type;
  const char *start; // points into the source string
  int length;
  int line;          // 1-based line number, for error messages
} Token;

// Point the lexer at a NUL-terminated source string.
void initLexer(const char *source);

// Save / restore the lexer's whole position. Used to lex an embedded expression
// (string interpolation) on a temporary buffer, then resume the outer source.
typedef struct {
  const char *source;
  const char *start;
  const char *current;
  int line;
} LexerState;
LexerState lexerSave(void);
void lexerRestore(LexerState state);
// The start of the source buffer currently being lexed, for rendering the line
// an error occurred on. NUL-terminated; valid while parsing that source.
const char *lexerSource(void);
// Begin numbering at `line` (used to band each file's lines for diagnostics).
void lexerSetLine(int line);
// Produce the next token. Called repeatedly by the parser until TOKEN_EOF.
Token scanToken(void);

#endif // CNANO_LEXER_H

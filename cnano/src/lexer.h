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
  TOKEN_LPAREN,
  TOKEN_RPAREN,
  // Literals
  TOKEN_NUMBER,
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
// Produce the next token. Called repeatedly by the parser until TOKEN_EOF.
Token scanToken(void);

#endif // CNANO_LEXER_H

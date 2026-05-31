#include "type.h"

const char *typeName(const Type *type) {
  switch (type->kind) {
  case TY_ANY:
    return "any";
  case TY_INT:
    return "int";
  case TY_BOOL:
    return "bool";
  case TY_STR:
    return "str";
  case TY_NIL:
    return "nil";
  case TY_FUNCTION:
    return "fn";
  default:
    return "?";
  }
}

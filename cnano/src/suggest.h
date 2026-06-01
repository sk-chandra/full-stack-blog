// suggest.h — "did you mean …?" name suggestions via edit distance.
//
// When a programmer writes an unknown name (a misspelt variable, type, field, or
// enum member), a good compiler guesses what they meant. This is a tiny, shared
// utility: Levenshtein distance plus a "closest candidate within a sensible
// threshold" helper, used by the type checker and the VM to enrich their errors.
#ifndef CNANO_SUGGEST_H
#define CNANO_SUGGEST_H

// The Levenshtein edit distance (insert/delete/substitute) between two
// NUL-terminated strings. Returns a large value for pathologically long inputs
// (names are short; we cap to keep it allocation-free).
int editDistance(const char *a, const char *b);

// The candidate closest to `target`, or NULL if none is close enough to be a
// plausible typo. The threshold scales with the target's length (short names
// tolerate fewer edits), so we never suggest a wildly different name.
const char *closestName(const char *target, const char *const *candidates,
                        int count);

#endif // CNANO_SUGGEST_H

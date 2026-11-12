/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
#include "lexer/token.h"

static const char *const kind_text[] = {
#define PLUM_TOKEN_TEXT(kind, text) text,
    PLUM_TOKEN_KINDS(PLUM_TOKEN_TEXT)
#undef PLUM_TOKEN_TEXT
};

static const char *const kind_name[] = {
#define PLUM_TOKEN_NAME(kind, text) #kind,
    PLUM_TOKEN_KINDS(PLUM_TOKEN_NAME)
#undef PLUM_TOKEN_NAME
};

const char *plum_token_kind_text(PlumTokenKind kind) {
    return (unsigned)kind < PLUM_TOKEN_KIND_COUNT ? kind_text[kind] : "?";
}

const char *plum_token_kind_name(PlumTokenKind kind) {
    return (unsigned)kind < PLUM_TOKEN_KIND_COUNT ? kind_name[kind] : "?";
}

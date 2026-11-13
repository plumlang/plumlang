/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Turning values into text, with the format options of interpolation
 * (spec 4.10): "{price:.2}", "{count:5}", "{name:<10}", "{n:x}".
 *
 *     format := [[fill] align] [width] ["." precision] [type]
 *     align  := "<" | ">" | "^"           (numbers default to >, text to <)
 *     type   := "d" | "f" | "e" | "x" | "X" | "o" | "b"
 *
 * A precision fixes a float's decimals, or cuts text to that many
 * characters. Widths count characters, not bytes.
 */
#ifndef PLUM_VM_TEXT_H
#define PLUM_VM_TEXT_H

#include "vm/value.h"

#include <stddef.h>

/*
 * The text of `v` under `format` (NULL or "" for plain text), as a new
 * string. On a format that doesn't fit the value, returns NULL and writes
 * the reason into `error`.
 */
PlumString *plum_value_text(PlumValue v, const char *format, char *error, size_t error_size);

/* The number of characters (code points) in UTF-8 text. */
size_t plum_utf8_length(const char *s, size_t len);

#endif /* PLUM_VM_TEXT_H */

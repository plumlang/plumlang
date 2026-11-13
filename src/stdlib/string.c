/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/*
 * The methods of str. Text is UTF-8; positions and lengths count
 * characters (code points), like indexing does. Changing case and the
 * is_ tests know ASCII letters, digits and spaces only.
 */
#include "stdlib/native.h"

#include "util/alloc.h"
#include "util/array.h"
#include "vm/text.h"

#include <string.h>

#define ARGS PlumNative *n, const PlumValue *args, int argc, PlumValue *result

typedef PLUM_ARRAY(char) Buf;

static const PlumString *S(PlumValue v) {
    return plum_as_string(v);
}

static PlumValue buf_value(Buf *b) {
    PlumValue v = plum_str_value(b->items != NULL ? b->items : "", b->len);
    plum_array_free(b);
    return v;
}

static void put(Buf *b, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        plum_array_push(b, s[i]);
    }
}

/* The byte length of the UTF-8 character starting with `c`. */
static size_t char_len(unsigned char c) {
    return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

static bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Where `part` first appears in `s` at or after byte `from`, or -1. */
static int64_t find_bytes(const PlumString *s, const PlumString *part, size_t from) {
    if (part->len == 0) {
        return (int64_t)from;
    }
    for (size_t i = from; i + part->len <= s->len; i++) {
        if (memcmp(s->chars + i, part->chars, part->len) == 0) {
            return (int64_t)i;
        }
    }
    return -1;
}

static bool s_upper(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]);
    PlumString *out = plum_string_alloc(s->len);
    for (size_t i = 0; i < s->len; i++) {
        char c = s->chars[i];
        out->chars[i] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool s_lower(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]);
    PlumString *out = plum_string_alloc(s->len);
    for (size_t i = 0; i < s->len; i++) {
        char c = s->chars[i];
        out->chars[i] = c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    *result = plum_obj(&out->obj);
    return true;
}

static PlumValue trimmed(const PlumString *s, bool start, bool end) {
    size_t a = 0, b = s->len;
    while (start && a < b && is_space(s->chars[a])) {
        a++;
    }
    while (end && b > a && is_space(s->chars[b - 1])) {
        b--;
    }
    return plum_str_value(s->chars + a, b - a);
}

static bool s_trim(ARGS) {
    (void)n;
    (void)argc;
    *result = trimmed(S(args[0]), true, true);
    return true;
}

static bool s_trim_start(ARGS) {
    (void)n;
    (void)argc;
    *result = trimmed(S(args[0]), true, false);
    return true;
}

static bool s_trim_end(ARGS) {
    (void)n;
    (void)argc;
    *result = trimmed(S(args[0]), false, true);
    return true;
}

/* split(): on runs of whitespace; split(sep): on each sep. */
static bool s_split(ARGS) {
    const PlumString *s = S(args[0]);
    const PlumString *sep = argc > 1 && args[1].type != PLUM_VAL_NONE ? S(args[1]) : NULL;
    PlumList *out = plum_list_new(0);
    if (sep == NULL) {
        size_t i = 0;
        while (i < s->len) {
            while (i < s->len && is_space(s->chars[i])) {
                i++;
            }
            size_t start = i;
            while (i < s->len && !is_space(s->chars[i])) {
                i++;
            }
            if (i > start) {
                plum_list_push(out, plum_str_value(s->chars + start, i - start));
            }
        }
    } else if (sep->len == 0) {
        plum_release(plum_obj(&out->obj));
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "split(\"\"): the separator is empty; use chars() for characters");
    } else {
        size_t start = 0;
        int64_t at;
        while ((at = find_bytes(s, sep, start)) >= 0) {
            plum_list_push(out, plum_str_value(s->chars + start, (size_t)at - start));
            start = (size_t)at + sep->len;
        }
        plum_list_push(out, plum_str_value(s->chars + start, s->len - start));
    }
    *result = plum_obj(&out->obj);
    return true;
}

/* lines(): split on \n (and \r\n), without a last empty line. */
static bool s_lines(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]);
    PlumList *out = plum_list_new(0);
    size_t start = 0;
    for (size_t i = 0; i <= s->len; i++) {
        if (i == s->len || s->chars[i] == '\n') {
            if (i == s->len && start == s->len) {
                break;
            }
            size_t end = i > start && s->chars[i - 1] == '\r' ? i - 1 : i;
            plum_list_push(out, plum_str_value(s->chars + start, end - start));
            start = i + 1;
        }
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool s_chars(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]);
    PlumList *out = plum_list_new(0);
    for (size_t i = 0; i < s->len;) {
        size_t k = char_len((unsigned char)s->chars[i]);
        if (i + k > s->len) {
            k = s->len - i;
        }
        plum_list_push(out, plum_str_value(s->chars + i, k));
        i += k;
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool s_contains(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(find_bytes(S(args[0]), S(args[1]), 0) >= 0);
    return true;
}

static bool s_starts_with(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]), *p = S(args[1]);
    *result = plum_bool(p->len <= s->len && memcmp(s->chars, p->chars, p->len) == 0);
    return true;
}

static bool s_ends_with(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]), *p = S(args[1]);
    *result =
        plum_bool(p->len <= s->len && memcmp(s->chars + s->len - p->len, p->chars, p->len) == 0);
    return true;
}

/* find(part): the character position of the first `part`, or none. */
static bool s_find(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]);
    int64_t at = find_bytes(s, S(args[1]), 0);
    *result = at < 0 ? plum_none() : plum_int((int64_t)plum_utf8_length(s->chars, (size_t)at));
    return true;
}

static bool s_count(ARGS) {
    (void)argc;
    const PlumString *s = S(args[0]), *part = S(args[1]);
    if (part->len == 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "count(\"\"): the text to count is empty");
    }
    int64_t count = 0;
    int64_t at = 0;
    while ((at = find_bytes(s, part, (size_t)at)) >= 0) {
        count++;
        at += (int64_t)part->len;
    }
    *result = plum_int(count);
    return true;
}

static bool s_replace(ARGS) {
    (void)argc;
    const PlumString *s = S(args[0]), *old = S(args[1]), *with = S(args[2]);
    if (old->len == 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE,
                                 "replace(\"\", ...): the text to replace is "
                                 "empty");
    }
    Buf b = {0};
    size_t start = 0;
    int64_t at;
    while ((at = find_bytes(s, old, start)) >= 0) {
        put(&b, s->chars + start, (size_t)at - start);
        put(&b, with->chars, with->len);
        start = (size_t)at + old->len;
    }
    put(&b, s->chars + start, s->len - start);
    *result = buf_value(&b);
    return true;
}

static bool s_repeat(ARGS) {
    (void)argc;
    const PlumString *s = S(args[0]);
    int64_t times = args[1].as.i;
    if (times < 0) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "repeat(%lld): the count must be 0 or more",
                                 (long long)times);
    }
    if (s->len > 0 && (uint64_t)times > (uint64_t)(SIZE_MAX / 2) / s->len) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "repeat(%lld): the text would be too long",
                                 (long long)times);
    }
    size_t len = plum_mul_size(s->len, (size_t)times);
    PlumString *out = plum_string_alloc(len);
    for (int64_t i = 0; i < times; i++) {
        memcpy(out->chars + (size_t)i * s->len, s->chars, s->len);
    }
    *result = plum_obj(&out->obj);
    return true;
}

static bool s_reverse(ARGS) {
    (void)n;
    (void)argc;
    const PlumString *s = S(args[0]);
    PlumString *out = plum_string_alloc(s->len);
    size_t at = s->len;
    for (size_t i = 0; i < s->len;) {
        size_t k = char_len((unsigned char)s->chars[i]);
        if (i + k > s->len) {
            k = s->len - i;
        }
        at -= k;
        memcpy(out->chars + at, s->chars + i, k); /* characters reversed, not bytes */
        i += k;
    }
    *result = plum_obj(&out->obj);
    return true;
}

/* pad_start / pad_end: up to `width` characters with `fill` (one character). */
static bool pad(PlumNative *n, const PlumValue *args, int argc, PlumValue *result, bool start) {
    const PlumString *s = S(args[0]);
    int64_t width = args[1].as.i;
    const PlumString *fill = argc > 2 && args[2].type != PLUM_VAL_NONE ? S(args[2]) : NULL;
    const char *f = fill != NULL ? fill->chars : " ";
    size_t flen = fill != NULL ? fill->len : 1;
    if (fill != NULL && plum_utf8_length(fill->chars, fill->len) != 1) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "pad: the fill must be one character");
    }
    size_t chars = plum_utf8_length(s->chars, s->len);
    if (width > 1000000000) {
        return plum_native_throw(n, PLUM_ERROR_VALUE, "pad: a width of %lld is too wide",
                                 (long long)width);
    }
    Buf b = {0};
    size_t missing = width > 0 && (size_t)width > chars ? (size_t)width - chars : 0;
    if (!start) {
        put(&b, s->chars, s->len);
    }
    for (size_t i = 0; i < missing; i++) {
        put(&b, f, flen);
    }
    if (start) {
        put(&b, s->chars, s->len);
    }
    *result = buf_value(&b);
    return true;
}

static bool s_pad_start(ARGS) {
    return pad(n, args, argc, result, true);
}

static bool s_pad_end(ARGS) {
    return pad(n, args, argc, result, false);
}

/* is_digit and friends: every character is one, and there is at least one. */
static bool all_of(const PlumString *s, int what) {
    if (s->len == 0) {
        return false;
    }
    for (size_t i = 0; i < s->len; i++) {
        char c = s->chars[i];
        bool ok = what == 0   ? c >= '0' && c <= '9'
                  : what == 1 ? (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                              : is_space(c);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static bool s_is_digit(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(all_of(S(args[0]), 0));
    return true;
}

static bool s_is_alpha(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(all_of(S(args[0]), 1));
    return true;
}

static bool s_is_space(ARGS) {
    (void)n;
    (void)argc;
    *result = plum_bool(all_of(S(args[0]), 2));
    return true;
}

static const PlumNativeFunction functions[] = {
    {"upper", "(self: str) -> str", s_upper, "the text in capitals (ASCII letters)"},
    {"lower", "(self: str) -> str", s_lower, "the text in small letters (ASCII letters)"},
    {"trim", "(self: str) -> str", s_trim, "without whitespace at either end"},
    {"trim_start", "(self: str) -> str", s_trim_start, "without whitespace at the start"},
    {"trim_end", "(self: str) -> str", s_trim_end, "without whitespace at the end"},
    {"split", "(self: str, separator: str = \"\") -> list[str]", s_split,
     "the pieces between separators; with none given, the words between whitespace"},
    {"lines", "(self: str) -> list[str]", s_lines, "the lines, without their line breaks"},
    {"chars", "(self: str) -> list[str]", s_chars, "each character as a str"},
    {"contains", "(self: str, part: str) -> bool", s_contains, "does the text contain part?"},
    {"starts_with", "(self: str, prefix: str) -> bool", s_starts_with,
     "does the text start with prefix?"},
    {"ends_with", "(self: str, suffix: str) -> bool", s_ends_with,
     "does the text end with suffix?"},
    {"find", "(self: str, part: str) -> int?", s_find, "the position of the first part, or none"},
    {"count", "(self: str, part: str) -> int", s_count, "how many times part appears"},
    {"replace", "(self: str, old: str, new: str) -> str", s_replace, "every old replaced with new"},
    {"repeat", "(self: str, times: int) -> str", s_repeat, "the text that many times over"},
    {"reverse", "(self: str) -> str", s_reverse, "the characters in reverse order"},
    {"pad_start", "(self: str, width: int, fill: str = \" \") -> str", s_pad_start,
     "filled at the start up to width characters"},
    {"pad_end", "(self: str, width: int, fill: str = \" \") -> str", s_pad_end,
     "filled at the end up to width characters"},
    {"is_digit", "(self: str) -> bool", s_is_digit, "is every character 0 to 9?"},
    {"is_alpha", "(self: str) -> bool", s_is_alpha, "is every character a letter (ASCII)?"},
    {"is_space", "(self: str) -> bool", s_is_space, "is every character whitespace?"},
};

const PlumNativeModule plum_module_str = {
    "str",     "the methods of str, always available on strings",
    functions, sizeof functions / sizeof functions[0],
    NULL,      0,
};

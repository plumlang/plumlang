/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
#include "util/semver.h"

#include <string.h>

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_ident_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

/* A number without leading zeros, at most 9 digits. Moves *s past it. */
static bool parse_number(const char **s, const char *end, uint32_t *out) {
    const char *p = *s;
    if (p >= end || !is_digit(*p) || (p[0] == '0' && p + 1 < end && is_digit(p[1]))) {
        return false;
    }
    uint32_t v = 0;
    int digits = 0;
    while (p < end && is_digit(*p)) {
        if (++digits > 9) {
            return false;
        }
        v = v * 10 + (uint32_t)(*p - '0');
        p++;
    }
    *s = p;
    *out = v;
    return true;
}

/*
 * Dot-separated identifiers from *s up to the first byte that can't be in
 * one: each non-empty, and with `numeric_rule` an all-digit one has no
 * leading zero. Moves *s past them.
 */
static bool parse_identifiers(const char **s, const char *end, bool numeric_rule) {
    const char *p = *s;
    for (;;) {
        const char *start = p;
        bool digits = true;
        while (p < end && is_ident_char(*p)) {
            digits = digits && is_digit(*p);
            p++;
        }
        if (p == start || (numeric_rule && digits && p - start > 1 && start[0] == '0')) {
            return false;
        }
        if (p < end && *p == '.') {
            p++;
            continue;
        }
        break;
    }
    *s = p;
    return true;
}

/*
 * One to three numbers, and with `full` (only after all three) a
 * prerelease and build. Returns how many numbers, or 0 if the text isn't
 * that.
 */
static int parse_partial(const char *text, const char *end, bool full, PlumVersion *out) {
    uint32_t parts[3] = {0, 0, 0};
    int n = 0;
    const char *p = text;
    while (n < 3) {
        if (!parse_number(&p, end, &parts[n])) {
            return 0;
        }
        n++;
        if (p >= end || *p != '.') {
            break;
        }
        p++;
    }
    memset(out, 0, sizeof *out);
    if (full && n == 3 && p < end && *p == '-') {
        const char *start = ++p;
        if (!parse_identifiers(&p, end, true)) {
            return 0;
        }
        out->pre = start;
        out->pre_len = (size_t)(p - start);
    }
    if (full && n == 3 && p < end && *p == '+') {
        const char *start = ++p;
        if (!parse_identifiers(&p, end, false)) {
            return 0;
        }
        out->build = start;
        out->build_len = (size_t)(p - start);
    }
    if (p != end) {
        return 0;
    }
    out->major = parts[0];
    out->minor = parts[1];
    out->patch = parts[2];
    return n;
}

bool plum_semver_parse(const char *text, size_t len, PlumVersion *out) {
    PlumVersion v;
    if (parse_partial(text, text + len, true, &v) != 3) {
        return false;
    }
    *out = v;
    return true;
}

bool plum_version_parse(const char *text, PlumVersion *out) {
    PlumVersion v;
    if (parse_partial(text, text + strlen(text), false, &v) != 3) {
        return false;
    }
    *out = v;
    return true;
}

/* Two prerelease identifiers, by the rules of semver.org item 11. */
static int compare_identifier(const char *a, size_t alen, const char *b, size_t blen) {
    bool an = true, bn = true;
    for (size_t i = 0; i < alen; i++) {
        an = an && is_digit(a[i]);
    }
    for (size_t i = 0; i < blen; i++) {
        bn = bn && is_digit(b[i]);
    }
    if (an && bn) {
        /* no leading zeros: the longer is the larger */
        if (alen != blen) {
            return alen < blen ? -1 : 1;
        }
        int c = memcmp(a, b, alen);
        return c < 0 ? -1 : c > 0;
    }
    if (an != bn) {
        return an ? -1 : 1; /* numbers come before words */
    }
    int c = memcmp(a, b, alen < blen ? alen : blen);
    if (c != 0) {
        return c < 0 ? -1 : 1;
    }
    return alen == blen ? 0 : alen < blen ? -1 : 1;
}

static int compare_prerelease(const char *a, size_t alen, const char *b, size_t blen) {
    size_t i = 0, j = 0;
    for (;;) {
        if (i >= alen || j >= blen) {
            return i >= alen && j >= blen ? 0 : i >= alen ? -1 : 1;
        }
        size_t ie = i, je = j;
        while (ie < alen && a[ie] != '.') {
            ie++;
        }
        while (je < blen && b[je] != '.') {
            je++;
        }
        int c = compare_identifier(a + i, ie - i, b + j, je - j);
        if (c != 0) {
            return c;
        }
        i = ie + 1;
        j = je + 1;
        if (ie >= alen || je >= blen) {
            return ie >= alen && je >= blen ? 0 : ie >= alen ? -1 : 1;
        }
    }
}

int plum_version_compare(PlumVersion a, PlumVersion b) {
    if (a.major != b.major) {
        return a.major < b.major ? -1 : 1;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor ? -1 : 1;
    }
    if (a.patch != b.patch) {
        return a.patch < b.patch ? -1 : 1;
    }
    bool ap = a.pre != NULL, bp = b.pre != NULL;
    if (ap != bp) {
        return ap ? -1 : 1; /* 1.0.0-alpha < 1.0.0 */
    }
    return ap ? compare_prerelease(a.pre, a.pre_len, b.pre, b.pre_len) : 0;
}

static PlumVersion version(uint32_t major, uint32_t minor, uint32_t patch) {
    PlumVersion v;
    memset(&v, 0, sizeof v);
    v.major = major;
    v.minor = minor;
    v.patch = patch;
    return v;
}

bool plum_semver_range_parse(const char *text, size_t len, bool prerelease, PlumRange *out) {
    const char *end = text + len;
    while (text < end && *text == ' ') {
        text++;
    }
    PlumRange r;
    memset(&r, 0, sizeof r);
    if (end - text == 1 && *text == '*') {
        *out = r;
        return true;
    }
    char op = '^';
    if (end - text >= 2 && text[0] == '>' && text[1] == '=') {
        op = '>';
        text += 2;
    } else if (text < end && (text[0] == '^' || text[0] == '~' || text[0] == '=')) {
        op = text[0];
        text++;
    }
    while (text < end && *text == ' ') {
        text++;
    }
    PlumVersion v;
    int parts = parse_partial(text, end, prerelease, &v);
    if (parts == 0) {
        return false;
    }
    r.low = v;
    r.has_high = op != '>';
    switch (op) {
    case '^': /* the left-most non-zero part stays */
        if (v.major > 0 || parts == 1) {
            r.high = version(v.major + 1, 0, 0);
        } else if (v.minor > 0 || parts == 2) {
            r.high = version(0, v.minor + 1, 0);
        } else {
            r.high = version(0, 0, v.patch + 1);
        }
        break;
    case '~': /* the patch moves; with only a major, the minor too */
        r.high = parts == 1 ? version(v.major + 1, 0, 0) : version(v.major, v.minor + 1, 0);
        break;
    case '=':
        r.high = parts == 3   ? version(v.major, v.minor, v.patch + 1)
                 : parts == 2 ? version(v.major, v.minor + 1, 0)
                              : version(v.major + 1, 0, 0);
        if (parts == 3 && v.pre != NULL) {
            r.high = v; /* exactly this prerelease: see plum_range_allows */
        }
        break;
    default:
        break;
    }
    *out = r;
    return true;
}

bool plum_range_parse(const char *text, PlumRange *out) {
    return plum_semver_range_parse(text, strlen(text), false, out);
}

bool plum_range_allows(const PlumRange *r, PlumVersion v) {
    if (v.pre != NULL && (r->low.pre == NULL || r->low.major != v.major ||
                          r->low.minor != v.minor || r->low.patch != v.patch)) {
        return false;
    }
    int low = plum_version_compare(v, r->low);
    if (low < 0) {
        return false;
    }
    if (r->has_high && r->high.pre != NULL) {
        return low == 0; /* =1.0.0-beta: that version only */
    }
    return !r->has_high || plum_version_compare(v, r->high) < 0;
}

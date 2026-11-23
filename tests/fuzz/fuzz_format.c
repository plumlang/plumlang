/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Fuzz target: format options of interpolation ("{x:>10.2f}", spec 4.10)
 * applied to values. The first byte picks the value, the next eight give
 * its number, and the rest, up to a zero byte, is the format; text after
 * that zero byte is the value when it is a string.
 */
#include "fuzz.h"

#include "vm/text.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 9) {
        return 0;
    }
    uint64_t bits = 0;
    memcpy(&bits, data + 1, 8);
    const char *rest = (const char *)data + 9;
    size_t rest_len = size - 9;
    const char *zero = memchr(rest, '\0', rest_len);
    size_t format_len = zero != NULL ? (size_t)(zero - rest) : rest_len;
    char *format = fuzz_text((const uint8_t *)rest, format_len);
    const char *text = zero != NULL ? zero + 1 : "";
    size_t text_len = zero != NULL ? rest_len - format_len - 1 : 0;

    PlumValue v;
    switch (data[0] % 6) {
    case 0:
        v = plum_int((int64_t)bits);
        break;
    case 1: {
        double f;
        memcpy(&f, &bits, sizeof f);
        v = plum_float(f);
        break;
    }
    case 2:
        v = plum_obj(&plum_string_new(text, text_len)->obj);
        break;
    case 3:
        v = plum_bool(bits & 1);
        break;
    case 4:
        v = plum_none();
        break;
    default: {
        PlumList *list = plum_list_new(0);
        plum_list_push(list, plum_int((int64_t)bits));
        plum_list_push(list, plum_obj(&plum_string_new(text, text_len)->obj));
        v = plum_obj(&list->obj);
        break;
    }
    }
    char error[256];
    PlumString *s = plum_value_text(v, format, error, sizeof error);
    if (s != NULL) {
        plum_release(plum_obj(&s->obj));
    }
    plum_release(v);
    free(format);
    return 0;
}

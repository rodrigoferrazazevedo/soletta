/*
 * This file is part of the Soletta Project
 *
 * Copyright (C) 2015 Intel Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef HAVE_LOCALE
#include <locale.h>
#endif

#include "sol-buffer.h"
#include "sol-util.h"
#include "sol-log.h"
#include "sol-random.h"
#include "sol-str-slice.h"

#if defined(HAVE_NEWLOCALE) && defined(HAVE_STRTOD_L)
static locale_t c_locale;
static void
clear_c_locale(void)
{
    freelocale(c_locale);
    c_locale = NULL;
}

static bool
init_c_locale(void)
{
    if (c_locale)
        return true;

    c_locale = newlocale(LC_ALL_MASK, "C", NULL);
    if (!c_locale)
        return false;

    atexit(clear_c_locale);
    return true;
}
#endif

void *
sol_util_memdup(const void *data, size_t len)
{
    void *ptr;

    ptr = malloc(len);
    if (ptr)
        memcpy(ptr, data, len);
    return ptr;
}

double
sol_util_strtodn(const char *nptr, char **endptr, ssize_t len, bool use_locale)
{
    char *tmpbuf, *tmpbuf_endptr;
    double value;

#if defined(HAVE_NEWLOCALE) && defined(HAVE_STRTOD_L)
    if (!use_locale) {
        if (!init_c_locale()) {
            /* not great, but a best effort to convert something */
            use_locale = false;
            SOL_WRN("could not create locale 'C', use current locale.");
        }
    }
#endif

    if (len < 0)
        len = (ssize_t)strlen(nptr);

    if (unlikely(len > (DBL_MANT_DIG - DBL_MIN_EXP + 3))) {
        errno = EINVAL;
        return FP_NAN;
    }

    /* NOTE: Using a copy to ensure trailing \0 and strtod() so we
     * properly parse numbers with large precision.
     *
     * Since parsing it is complex (ie:
     * http://www.netlib.org/fp/dtoa.c), we take the short path to
     * call libc.
     */
    tmpbuf = strndupa(nptr, len);

    errno = 0;
#ifdef HAVE_NEWLOCALE
    if (!use_locale) {
#ifdef HAVE_STRTOD_L
        value = strtod_l(tmpbuf, &tmpbuf_endptr, c_locale);
#else
        /* fallback to query locale's decimal point and if it's
         * different than '.' we replace JSON's '.' with locale's
         * decimal point if the given number contains a '.'.
         *
         * We also replace any existing locale decimal_point with '.'
         * so it will return correct endptr.
         *
         * Extra care since decimal point may be multi-byte.
         */
        struct lconv *lc = localeconv();
        if (lc && lc->decimal_point && !streq(lc->decimal_point, ".")) {
            if (strchr(tmpbuf, '.') || strstr(tmpbuf, lc->decimal_point)) {
                int dplen = strlen(lc->decimal_point);
                const char *src, *src_end = tmpbuf + len;
                char *dst;
                if (dplen == 1) {
                    for (src = tmpbuf, dst = tmpbuf; src < src_end; src++, dst++) {
                        if (*src == '.')
                            *dst = *lc->decimal_point;
                        else if (*src == *lc->decimal_point)
                            *dst = '.';
                    }
                } else {
                    char *transl;
                    unsigned count = 0;

                    for (src = tmpbuf; src < src_end; src++) {
                        if (*src == '.')
                            count++;
                    }

                    transl = alloca(len + (count * dplen) + 1);
                    for (src = tmpbuf, dst = transl; src < src_end;) {
                        if (*src == '.') {
                            memcpy(dst, lc->decimal_point, dplen);
                            dst += dplen;
                            src++;
                        } else if ((src_end - src) >= dplen &&
                            streqn(src, lc->decimal_point, dplen)) {
                            *dst = '.';
                            dst++;
                            src += dplen;
                        } else {
                            *dst = *src;
                            dst++;
                            src++;
                        }
                    }
                    *dst = '\0';
                    tmpbuf = transl;
                }
            }
        }
        value = strtod(tmpbuf, &tmpbuf_endptr);
#endif
    } else
#endif
    {
        value = strtod(tmpbuf, &tmpbuf_endptr);
    }

    if (endptr)
        *endptr = (char *)nptr + (tmpbuf_endptr - tmpbuf);

    return value;
}

char *
sol_util_strerror(int errnum, char *buf, size_t buflen)
{
    char *ret;

    if (buflen < 1)
        return NULL;

    buf[0] = '\0';

    ret = (char *)strerror_r(errnum, buf, buflen);
    /* if buf was used it means it can be XSI version (so ret won't be
       pointing to msg string), or GNU version using non static string
       (in this case ret == buf already) */
    if (buf[0] != '\0')
        ret = buf;

    return ret;
}

struct sol_vector
sol_util_str_split(const struct sol_str_slice slice,
    const char *delim,
    size_t maxsplit)
{
    struct sol_vector v = SOL_VECTOR_INIT(struct sol_str_slice);
    const char *str = slice.data;
    ssize_t dlen;
    size_t len;

    if (!slice.len || !delim)
        return v;

    maxsplit = (maxsplit) ? : slice.len - 1;
    dlen = strlen(delim);
    len = slice.len;

#define CREATE_SLICE(_str, _len) \
    do { \
        s = sol_vector_append(&v); \
        if (!s) \
            goto err; \
        s->data = _str; \
        s->len = _len; \
    } while (0)

    while (str && (v.len < maxsplit + 1)) {
        struct sol_str_slice *s;
        char *token = memmem(str, len, delim, dlen);
        if (!token) {
            CREATE_SLICE(str, len);
            break;
        }

        if (v.len == (uint16_t)maxsplit)
            CREATE_SLICE(str, len);
        else
            CREATE_SLICE(str, (size_t)(token - str));

        len -= (token - str) + dlen;
        str = token + dlen;
    }
#undef CREATE_SLICE

    return v;

err:
    sol_vector_clear(&v);
    return v;
}

static struct sol_uuid
assert_uuid_v4(struct sol_uuid id)
{
    id.bytes[6] = (id.bytes[6] & 0x0F) | 0x40;
    id.bytes[8] = (id.bytes[8] & 0x3F) | 0x80;

    return id;
}

static int
uuid_gen(struct sol_uuid *ret)
{
    struct sol_buffer buf = SOL_BUFFER_INIT_FLAGS(ret, sizeof(*ret),
        SOL_BUFFER_FLAGS_MEMORY_NOT_OWNED | SOL_BUFFER_FLAGS_NO_NUL_BYTE);
    struct sol_random *engine;
    ssize_t size;

    SOL_NULL_CHECK(ret, -EINVAL);
    engine = sol_random_new(SOL_RANDOM_DEFAULT, 0);
    SOL_NULL_CHECK(engine, -errno);

    size = sol_random_fill_buffer(engine, &buf, sizeof(*ret));
    sol_random_del(engine);
    sol_buffer_fini(&buf);

    if (size != (ssize_t)sizeof(*ret))
        return -EIO;

    *ret = assert_uuid_v4(*ret);
    return 0;
}

// 37 = 2 * 16 (chars) + 4 (hyphens) + 1 (\0)
int
sol_util_uuid_gen(bool upcase,
    bool with_hyphens,
    char id[static 37])
{
    static struct sol_str_slice hyphen = SOL_STR_SLICE_LITERAL("-");
    /* hyphens on positions 8, 13, 18, 23 (from 0) */
    static const int hyphens_pos[] = { 8, 13, 18, 23 };
    struct sol_uuid uuid = { 0 };
    unsigned i;
    int r;

    struct sol_buffer buf = { 0 };

    sol_buffer_init_flags(&buf, id, 37,
        SOL_BUFFER_FLAGS_MEMORY_NOT_OWNED);

    r = uuid_gen(&uuid);
    SOL_INT_CHECK(r, < 0, r);

    for (i = 0; i < ARRAY_SIZE(uuid.bytes); i++) {
        r = sol_buffer_append_printf(&buf, upcase ? "%02hhX" : "%02hhx",
            uuid.bytes[i]);
        SOL_INT_CHECK_GOTO(r, < 0, err);
    }

    if (with_hyphens) {
        for (i = 0; i < ARRAY_SIZE(hyphens_pos); i++) {
            r = sol_buffer_insert_slice(&buf, hyphens_pos[i], hyphen);
            SOL_INT_CHECK_GOTO(r, < 0, err);
        }
    }

err:
    sol_buffer_fini(&buf);
    return r;
}

SOL_API int
sol_util_replace_str_if_changed(char **str, const char *new_str)
{
    SOL_NULL_CHECK(str, -EINVAL);

    if (!new_str) {
        free(*str);
        *str = NULL;
        return 0;
    }

    if (*str) {
        size_t str_len = strlen(*str);
        size_t new_str_len = strlen(new_str);

        if (str_len == new_str_len && memcmp(*str, new_str, str_len) == 0) {
            return 0;
        } else if (str_len < new_str_len) {
            char *tmp = realloc(*str, new_str_len + 1);
            SOL_NULL_CHECK(tmp, -ENOMEM);
            *str = tmp;
        }
        memcpy(*str, new_str, new_str_len + 1);
    } else {
        *str = strdup(new_str);
        SOL_NULL_CHECK(*str, -ENOMEM);
    }

    return 0;
}

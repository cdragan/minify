/* SPDX-License-Identifier: MIT
 * Copyright (c) 2022 Chris Dragan
 */

#include "find_repeats.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

enum WHAT {
    w_end,
    w_literal,
    w_MATCH
};

typedef struct {
    enum WHAT what;
    size_t    pos;
    size_t    size;
    size_t    offset;
} EXPECT;

typedef struct {
    const char *buf;
    size_t      size;
    int         line;
    EXPECT     *expect;
} TEST_CASE;

static const char *what_str(enum WHAT what)
{
    switch (what) {
        case w_end:     return "end";
        case w_literal: return "literal";
        case w_MATCH:   return "match";
        default:        break;
    }
    assert(0);
    return "unknown";
}

static void test_report(void *cookie, enum WHAT what, size_t pos, size_t size, size_t offset)
{
    TEST_CASE *test_case = (TEST_CASE *)cookie;
    EXPECT    *expect    = test_case->expect;

    if ( ! expect)
        return;

    if (expect->what != what) {
        fprintf(stderr, "test_repeats.c:%d: expected %s but got %s\n",
                test_case->line, what_str(expect->what), what_str(what));
        test_case->expect = NULL;
        return;
    }

    assert(what == w_literal || what == w_MATCH);

    assert((what != w_literal) || (expect->offset == 0));

    if (expect->pos != pos) {
        fprintf(stderr, "test_repeats.c:%d: expected pos %zu but got %zu\n",
                test_case->line, expect->pos, pos);
        test_case->expect = NULL;
        return;
    }

    if (expect->size != size) {
        fprintf(stderr, "test_repeats.c:%d: expected size %zu but got %zu\n",
                test_case->line, expect->size, size);
        test_case->expect = NULL;
        return;
    }

    if (what == w_MATCH && expect->offset != offset) {
        fprintf(stderr, "test_repeats.c:%d: expected offset %zu but got %zu\n",
                test_case->line, expect->offset, offset);
        test_case->expect = NULL;
        return;
    }

    ++test_case->expect;
}

static void report_literal(void *cookie, const char *buf, size_t pos, size_t size)
{
    test_report(cookie, w_literal, pos, size, 0);
}

static void report_match(void *cookie, const char *buf, size_t pos, OCCURRENCE occurrence)
{
    test_report(cookie, w_MATCH, pos, occurrence.length, occurrence.offset);
}

static unsigned run_test(const char *buf, size_t size, int line, EXPECT *expect)
{
    TEST_CASE test_case = { buf, size, line, expect };

    const int err = find_repeats(buf, size, report_literal, report_match, &test_case);

    if (err) {
        fprintf(stderr, "test_repeats.c:%d: find_repeats failed with %d\n",
                line, err);
        return 1;
    }

    if ( ! test_case.expect)
        return 1;

    if (test_case.expect->what != w_end) {
        const EXPECT *last_expect = test_case.expect;

        fprintf(stderr, "test_repeats.c:%d: expected test case was not found: %s pos %zu size %zu\n",
                line, what_str(last_expect->what), last_expect->pos, last_expect->size);
        return 1;
    }

    return 0;
}

#define RUN_TEST(test_str, expect) { \
    static const char buf[] = test_str; \
    num_failed += run_test(buf, sizeof(buf) - 1, __LINE__, expect); \
}

int main(void)
{
    unsigned num_failed = 0;

    /* Empty buffer */
    {
        static EXPECT expect[] = {
            { w_end, 0, 0, 0 }
        };
        RUN_TEST("", expect);
    }

    /* One byte */
    {
        static EXPECT expect[] = {
            { w_literal, 0, 1, 0 },
            { w_end,     0, 0, 0 }
        };
        RUN_TEST("a", expect);
    }

    /* Three literal bytes */
    {
        static EXPECT expect[] = {
            { w_literal, 0, 3, 0 },
            { w_end,     0, 0, 0 }
        };
        RUN_TEST("abc", expect);
    }

    /* Repetition of size 1 */
    {
        static EXPECT expect[] = {
            { w_literal, 0, 2, 0 },
            { w_MATCH,   2, 3, 1 },
            { w_literal, 5, 1, 0 },
            { w_end,     0, 0, 0 }
        };
        RUN_TEST("abbbbc", expect);
    }

    /* Repetition of size 2 */
    {
        static EXPECT expect[] = {
            { w_literal, 0, 3, 0 },
            { w_MATCH,   3, 2, 2 },
            { w_end,     0, 0, 0 }
        };
        RUN_TEST("abcbc", expect);
    }

    /* Use longest repetition */
    {
        static EXPECT expect[] = {
            { w_literal,  0, 5,  0 },
            { w_MATCH,    5, 2,  3 },
            { w_literal,  7, 1,  0 },
            { w_MATCH,    8, 2,  7 },
            { w_literal, 10, 1,  0 },
            { w_MATCH,   11, 3, 10 },
            { w_end,      0, 0,  0 }
        };
        RUN_TEST("0bcd1cd2bc3bcd", expect);
    }

    /* Use longest repetition */
    {
        static EXPECT expect[] = {
            { w_literal, 0, 4, 0 },
            { w_MATCH,   4, 2, 3 },
            { w_literal, 6, 2, 0 },
            { w_MATCH,   8, 3, 4 },
            { w_end,     0, 0, 0 }
        };
        RUN_TEST("0bc1bcd2bcd", expect);
    }

    /* Prefer smallest offset */
    {
        static EXPECT expect[] = {
            { w_literal, 0, 4, 0 },
            { w_MATCH,   4, 3, 4 },
            { w_MATCH,   7, 3, 3 },
            { w_end,     0, 0, 0 }
        };
        RUN_TEST("abc abcabc", expect);
    }

    /* Prefer same offset as last time */
    {
        static EXPECT expect[] = {
            { w_literal,  0, 7,  0 },
            { w_MATCH,    7, 3,  4 },
            { w_MATCH,   10, 2, 10 },
            { w_literal, 12, 1,  0 },
            { w_MATCH,   13, 3, 10 },
            { w_end,      0, 0,  0 }
        };
        RUN_TEST("dexabc abcdeyabc", expect);
    }

    if (num_failed)
        fprintf(stderr, "test_repeats.c: failed %u tests\n", num_failed);

    return num_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

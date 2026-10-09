/* A tiny single-header test framework: no network fetch, no vendoring, just enough to run
 * assertions and report a pass/fail summary for CTest. */
#ifndef MINITEST_H
#define MINITEST_H

#include <stdio.h>

static int mt_checks = 0;
static int mt_failures = 0;

#define MT_CHECK(cond)                                                                   \
    do {                                                                                 \
        mt_checks++;                                                                     \
        if (!(cond)) {                                                                   \
            mt_failures++;                                                               \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                                 \
    } while (0)

#define MT_RUN(test_fn)                                                                  \
    do {                                                                                 \
        fprintf(stderr, "-- %s\n", #test_fn);                                            \
        test_fn();                                                                       \
    } while (0)

static inline int mt_summary(void)
{
    fprintf(stderr, "%d checks, %d failed\n", mt_checks, mt_failures);
    return (mt_failures == 0) ? 0 : 1;
}

#endif /* MINITEST_H */

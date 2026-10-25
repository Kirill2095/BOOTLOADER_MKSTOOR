/* SPDX-License-Identifier: AGPL-3.0-only */
/* MKSTOOR — tiny host-side unit test harness (Meson `native` suite). */
#ifndef MKSTOOR_TEST_FRAMEWORK_H
#define MKSTOOR_TEST_FRAMEWORK_H
#include <stdio.h>
#include <string.h>

static int g_tests_run, g_tests_failed;

#define CHECK(cond) do {                                                 \
        g_tests_run++;                                                   \
        if (!(cond)) {                                                   \
                g_tests_failed++;                                        \
                printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                \
} while (0)

#define TEST_SUMMARY()                                                   \
        (printf("%d checks, %d failed\n", g_tests_run, g_tests_failed),  \
         g_tests_failed == 0 ? 0 : 1)

#endif /* MKSTOOR_TEST_FRAMEWORK_H */

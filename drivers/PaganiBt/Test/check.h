/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* 主机测试的检查宏：失败时打印位置并结束这个测试。 */
#ifndef PAGANI_BT_TEST_CHECK_H
#define PAGANI_BT_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
#define CHECK_BYTES(got, n, ...) do { \
        const unsigned char want_[] = {__VA_ARGS__}; \
        CHECK((size_t)(n) == sizeof want_); \
        CHECK(!memcmp((got), want_, sizeof want_)); \
    } while (0)

#endif

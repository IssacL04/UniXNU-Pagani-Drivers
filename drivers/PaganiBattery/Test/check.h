/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The host tests' check macro: a failing check prints its place and stops the test. */
#ifndef PAGANI_BATTERY_TEST_CHECK_H
#define PAGANI_BATTERY_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

#endif

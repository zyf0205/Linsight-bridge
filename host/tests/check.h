#pragma once

#include <stdio.h>
#include <stdlib.h>

/* Always evaluate checks, including fixture setup, in Release builds. */
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            exit(EXIT_FAILURE);                                                \
        }                                                                      \
    } while (0)

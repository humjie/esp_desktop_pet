#pragma once

#include <stddef.h>

typedef struct {
    const char *text;
    const char *category;
} quote_entry_t;

/* Generated at build time; the table and strings remain in flash. */
extern const quote_entry_t deskpet_quotes[];
extern const size_t deskpet_quote_count;

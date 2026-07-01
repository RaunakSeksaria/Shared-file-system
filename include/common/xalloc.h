#ifndef XALLOC_H
#define XALLOC_H

// Abort-on-OOM allocation wrappers. Allocation failure in this system is
// unrecoverable, so the wrappers fail loudly to stderr and abort() instead of
// forcing every call site to handle NULL. Header-only (static inline) so no
// extra translation unit is needed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (malloc %zu bytes)\n", n);
        abort();
    }
    return p;
}

static inline void *xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (realloc %zu bytes)\n", n);
        abort();
    }
    return p;
}

static inline void *xcalloc(size_t nmemb, size_t size) {
    void *p = calloc(nmemb, size);
    if (!p) {
        fprintf(stderr, "fatal: out of memory (calloc %zu x %zu bytes)\n", nmemb, size);
        abort();
    }
    return p;
}

static inline char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = (char *)xmalloc(n);
    memcpy(d, s, n);
    return d;
}

#endif

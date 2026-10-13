#ifndef EMACS_IO_STRBUF_H
#define EMACS_IO_STRBUF_H

#include "emacs-module.h"
#include <stdbool.h>
#include <stddef.h>

/* ── growable byte buffer ──────────────────────────────────────────────── */

typedef struct {
  char   *data;
  size_t  len;
  size_t  cap;
} strbuf_t;

strbuf_t *strbuf_create(size_t initial_cap);
void      strbuf_free(strbuf_t *sb);
bool      strbuf_append(strbuf_t *sb, const char *s, ptrdiff_t len);
emacs_value strbuf_to_estring(emacs_env *env, strbuf_t *sb);

#endif /* EMACS_IO_STRBUF_H */

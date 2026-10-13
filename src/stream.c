#include "stream.h"
#include <stdlib.h>
#include <string.h>

/* ── dispatch helpers ────────────────────────────────────────────────── */

#define LOCK(s)   pthread_mutex_lock(&(s)->mutex)
#define UNLOCK(s) pthread_mutex_unlock(&(s)->mutex)
#define RESOLVE(s) do { if ((s)->ops && (s)->ops->resolve) (s)->ops->resolve(s); } while (0)

/* ── shared in-memory cursor ─────────────────────────────────────────── */

static mem_cursor_t *mem_cursor(cl_stream_t *s) {
  return s ? (mem_cursor_t *)s->impl : NULL;
}

ptrdiff_t mem_cursor_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  mem_cursor_t *c = mem_cursor(s);
  if (!c || !c->data || c->pos >= c->len || n <= 0) return 0;
  size_t remaining = c->len - c->pos;
  size_t to_copy = (size_t)n < remaining ? (size_t)n : remaining;
  memcpy(buf, c->data + c->pos, to_copy);
  c->pos += to_copy;
  return (ptrdiff_t)to_copy;
}

bool mem_cursor_direct(cl_stream_t *s, const void **addr, size_t *len) {
  mem_cursor_t *c = mem_cursor(s);
  if (!c || !c->data || c->pos >= c->len) return false;
  *addr = c->data + c->pos;
  *len = c->len - c->pos;
  return true;
}

bool mem_cursor_listen(cl_stream_t *s) {
  mem_cursor_t *c = mem_cursor(s);
  return c && c->data && c->pos < c->len;
}

ptrdiff_t mem_cursor_file_position(cl_stream_t *s) {
  mem_cursor_t *c = mem_cursor(s);
  return c ? (ptrdiff_t)c->pos : -1;
}

bool mem_cursor_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  mem_cursor_t *c = mem_cursor(s);
  if (!c || pos < 0 || (size_t)pos > c->len) return false;
  c->pos = (size_t)pos;
  return true;
}

ptrdiff_t mem_cursor_file_length(cl_stream_t *s) {
  mem_cursor_t *c = mem_cursor(s);
  return c ? (ptrdiff_t)c->len : -1;
}

void mem_cursor_advance(cl_stream_t *s, ptrdiff_t n) {
  mem_cursor_t *c = mem_cursor(s);
  if (!c || n <= 0) return;
  c->pos += (size_t)n;
  if (c->pos > c->len) c->pos = c->len;
}

void mem_cursor_clear_input(cl_stream_t *s) {
  mem_cursor_t *c = mem_cursor(s);
  if (c) c->pos = c->len;
}

/* ── live-stream registry ────────────────────────────────────────────── */

static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static cl_stream_t **registry;
static size_t registry_len, registry_cap;

void stream_registry_add(cl_stream_t *s) {
  if (!s) return;
  pthread_mutex_lock(&registry_mutex);
  if (registry_len == registry_cap) {
    size_t ncap = registry_cap ? registry_cap * 2 : 16;
    cl_stream_t **n = realloc(registry, ncap * sizeof(*n));
    if (n) { registry = n; registry_cap = ncap; }
  }
  if (registry_len < registry_cap)
    registry[registry_len++] = s;
  pthread_mutex_unlock(&registry_mutex);
}

void stream_registry_remove(cl_stream_t *s) {
  if (!s) return;
  pthread_mutex_lock(&registry_mutex);
  for (size_t i = 0; i < registry_len; i++) {
    if (registry[i] == s) {
      registry[i] = registry[--registry_len];
      break;
    }
  }
  pthread_mutex_unlock(&registry_mutex);
}

bool stream_registered(cl_stream_t *s) {
  if (!s) return false;
  bool found = false;
  pthread_mutex_lock(&registry_mutex);
  for (size_t i = 0; i < registry_len; i++) {
    if (registry[i] == s) { found = true; break; }
  }
  pthread_mutex_unlock(&registry_mutex);
  return found;
}

/* ── construction ────────────────────────────────────────────────────── */

cl_stream_t *stream_alloc(const cl_stream_ops_t *ops, void *impl,
                          cl_stream_kind_t kind, unsigned flags) {
  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->ops = ops;
  s->impl = impl;
  s->kind = kind;
  s->flags = flags;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);
  stream_registry_add(s);
  return s;
}

ptrdiff_t stream_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0 || !s->ops || !s->ops->read) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);

  /* drain pushback ring buffer first */
  if (s->pushback_head != s->pushback_tail) {
    ptrdiff_t count = 0;
    unsigned char *dest = (unsigned char *)buf;
    while (s->pushback_head != s->pushback_tail && count < n) {
      dest[count++] = (unsigned char)s->pushback_chars[s->pushback_head];
      s->pushback_head = (s->pushback_head + 1) & (CL_PUSHBACK_SIZE - 1);
    }
    UNLOCK(s);
    return count;
  }

  ptrdiff_t r = s->ops->read(s, buf, n);
  UNLOCK(s);
  return r;
}

/* Advance the output column/last-char bookkeeping by N written bytes,
   restarting the column count after the last newline (for cl-fresh-line). */
void stream_update_column(cl_stream_t *s, const unsigned char *p, ptrdiff_t n) {
  ptrdiff_t after_nl = 0;
  bool found_nl = false;
  for (ptrdiff_t i = n - 1; i >= 0; i--) {
    if (p[i] == '\n') { after_nl = n - 1 - i; found_nl = true; break; }
  }
  s->column = found_nl ? after_nl : s->column + n;
  s->last_char = (int)p[n - 1];
}

ptrdiff_t stream_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0 || !s->ops || !s->ops->write) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  ptrdiff_t r = s->ops->write(s, buf, n);
  if (r > 0)
    stream_update_column(s, (const unsigned char *)buf, r);
  UNLOCK(s);
  return r;
}

int stream_close(cl_stream_t *s) {
  if (!s) return -1;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  int r = 0;
  if (s->ops && s->ops->close)
    r = s->ops->close(s);
  else
    atomic_store(&s->open, false);
  UNLOCK(s);
  return r;
}

int stream_flush(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->flush) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  int r = s->ops->flush(s);
  UNLOCK(s);
  return r;
}

int stream_finish(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->finish) return 0;
  if (!atomic_load(&s->open)) return 0;
  RESOLVE(s);
  LOCK(s);
  int r = s->ops->finish(s);
  UNLOCK(s);
  return r;
}

bool stream_listen(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->listen) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  if (s->pushback_head != s->pushback_tail) { UNLOCK(s); return true; }
  bool r = s->ops->listen(s);
  UNLOCK(s);
  return r;
}

void stream_unread(cl_stream_t *s, int ch) {
  if (!s) return;
  LOCK(s);
  int next = (s->pushback_tail + 1) & (CL_PUSHBACK_SIZE - 1);
  if (next == s->pushback_head) { UNLOCK(s); return; }
  s->pushback_chars[s->pushback_tail] = ch;
  s->pushback_tail = next;
  UNLOCK(s);
}

void stream_unread_bulk(cl_stream_t *s, const unsigned char *data, ptrdiff_t n) {
  if (!s || !data || n <= 0) return;
  LOCK(s);
  for (ptrdiff_t i = 0; i < n; i++) {
    int next = (s->pushback_tail + 1) & (CL_PUSHBACK_SIZE - 1);
    if (next == s->pushback_head) break;   /* ring full: drop the rest */
    s->pushback_chars[s->pushback_tail] = data[i];
    s->pushback_tail = next;
  }
  UNLOCK(s);
}

ptrdiff_t stream_read_pushback(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  LOCK(s);
  ptrdiff_t count = 0;
  unsigned char *dest = (unsigned char *)buf;
  while (s->pushback_head != s->pushback_tail && count < n) {
    dest[count++] = (unsigned char)s->pushback_chars[s->pushback_head];
    s->pushback_head = (s->pushback_head + 1) & (CL_PUSHBACK_SIZE - 1);
  }
  UNLOCK(s);
  return count;
}

bool stream_direct(cl_stream_t *s, const void **addr, size_t *len) {
  if (!s || !s->ops || !s->ops->direct || !addr || !len) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  bool ok = s->ops->direct(s, addr, len);
  UNLOCK(s);
  return ok;
}

void stream_advance(cl_stream_t *s, ptrdiff_t n) {
  if (!s || n <= 0 || !s->ops || !s->ops->advance) return;
  RESOLVE(s);
  LOCK(s);
  s->ops->advance(s, n);
  UNLOCK(s);
}

ptrdiff_t stream_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  if (!s || !env) return 0;
  if (!atomic_load(&s->open)) return 0;

  if (s->ops && s->ops->write_string) {
    RESOLVE(s);
    LOCK(s);
    ptrdiff_t r = s->ops->write_string(env, s, str);
    UNLOCK(s);
    return r;
  }

  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, str, NULL, &slen)) return 0;
  if (slen == 0) return 0;

  unsigned char *buf = malloc((size_t)slen);
  if (!buf) return -1;

  ptrdiff_t len = slen;
  if (!env->copy_string_contents(env, str, (char *)buf, &len)) {
    free(buf);
    return -1;
  }
  len--;

  ptrdiff_t r = stream_write(s, buf, len);
  free(buf);
  return r;
}

ptrdiff_t stream_file_position(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->file_position) return -1;
  if (!atomic_load(&s->open)) return -1;
  RESOLVE(s);
  LOCK(s);
  ptrdiff_t r = s->ops->file_position(s);
  UNLOCK(s);
  return r;
}

bool stream_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  if (!s || pos < 0 || !s->ops || !s->ops->set_file_position) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  bool r = s->ops->set_file_position(s, pos);
  UNLOCK(s);
  return r;
}

ptrdiff_t stream_file_length(cl_stream_t *s) {
  if (!s || !s->ops || !s->ops->file_length) return -1;
  RESOLVE(s);
  LOCK(s);
  ptrdiff_t r = s->ops->file_length(s);
  UNLOCK(s);
  return r;
}

bool stream_truncate(cl_stream_t *s, ptrdiff_t len) {
  if (!s || len < 0 || !s->ops || !s->ops->truncate) return false;
  if (!atomic_load(&s->open)) return false;
  RESOLVE(s);
  LOCK(s);
  bool r = s->ops->truncate(s, len);
  UNLOCK(s);
  return r;
}

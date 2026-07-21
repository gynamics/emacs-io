#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── string-input ops ────────────────────────────────────────────────── */

static ptrdiff_t sis_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       sis_close(cl_stream_t *s);
static bool      sis_listen(cl_stream_t *s);
static void      sis_destroy(cl_stream_t *s);

static const cl_stream_ops_t string_input_ops = {
  .read    = sis_read,
  .write   = NULL,
  .close   = sis_close,
  .flush   = NULL,
  .finish  = NULL,
  .listen  = sis_listen,
  .destroy = sis_destroy,
};

/* ── string-output ops ───────────────────────────────────────────────── */

static ptrdiff_t sos_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static int       sos_close(cl_stream_t *s);
static void      sos_destroy(cl_stream_t *s);

static const cl_stream_ops_t string_output_ops = {
  .read    = NULL,
  .write   = sos_write,
  .close   = sos_close,
  .flush   = NULL,
  .finish  = NULL,
  .listen  = NULL,
  .destroy = sos_destroy,
};

/* ── string input: create / read / close / listen ────────────────────── */

cl_stream_t *stream_create_string_input(const char *data, size_t len) {
  string_input_impl_t *si = malloc(sizeof(*si));
  if (!si) return NULL;
  si->data = data;
  si->len = len;
  si->pos = 0;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(si); return NULL; }

  s->ops = &string_input_ops;
  s->impl = si;
  s->kind = CL_STREAM_KIND_STRING_INPUT;
  s->flags = CL_STREAM_FLAG_INPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

static ptrdiff_t sis_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || si->pos >= si->len) return 0; /* EOF */

  size_t remaining = si->len - si->pos;
  size_t to_copy = (size_t)n < remaining ? (size_t)n : remaining;
  memcpy(buf, si->data + si->pos, to_copy);
  si->pos += to_copy;
  return (ptrdiff_t)to_copy;
}

static int sis_close(cl_stream_t *s) {
  if (!atomic_load(&s->open)) return 0;
  atomic_store(&s->open, false);
  return 0;
}

static void sis_destroy(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (si) {
    free((void *)si->data);
    free(si);
  }
}

static bool sis_listen(cl_stream_t *s) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  return si != NULL && si->pos < si->len;
}

/* ── string output: create / write / close ────────────────────────────── */

cl_stream_t *stream_create_string_output(void) {
  string_output_impl_t *so = malloc(sizeof(*so));
  if (!so) return NULL;
  so->cap = STRING_OUTPUT_INITIAL_CAP;
  so->data = malloc(so->cap);
  if (!so->data) { free(so); return NULL; }
  so->len = 0;
  so->data[0] = '\0';

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { free(so->data); free(so); return NULL; }

  s->ops = &string_output_ops;
  s->impl = so;
  s->kind = CL_STREAM_KIND_STRING_OUTPUT;
  s->flags = CL_STREAM_FLAG_OUTPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

static ptrdiff_t sos_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  string_output_impl_t *so = (string_output_impl_t *)s->impl;
  if (!so) return -1;

  size_t needed = so->len + (size_t)n + 1;
  if (needed > so->cap) {
    size_t new_cap = so->cap;
    while (new_cap < needed) new_cap = (new_cap * 3) / 2;
    char *new_data = realloc(so->data, new_cap);
    if (!new_data) return -1;
    so->data = new_data;
    so->cap = new_cap;
  }
  memcpy(so->data + so->len, buf, (size_t)n);
  so->len += (size_t)n;
  so->data[so->len] = '\0';
  return n;
}

static int sos_close(cl_stream_t *s) {
  if (!atomic_load(&s->open)) return 0;
  atomic_store(&s->open, false);
  return 0;
}

static void sos_destroy(cl_stream_t *s) {
  string_output_impl_t *so = (string_output_impl_t *)s->impl;
  if (so) {
    free(so->data);
    free(so);
  }
}

/* ── buffer input stream ─────────────────────────────────────────────── */

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       bis_close(cl_stream_t *s);
static bool      bis_listen(cl_stream_t *s);
static void      bis_destroy(cl_stream_t *s);

static const cl_stream_ops_t buffer_input_ops = {
  .read    = bis_read,
  .write   = NULL,
  .close   = bis_close,
  .flush   = NULL,
  .finish  = NULL,
  .listen  = bis_listen,
  .destroy = bis_destroy,
};

typedef struct {
  emacs_env  *env;
  emacs_value buffer_ref;
  ptrdiff_t   pos;
  ptrdiff_t   end;
  char       *chunk;
  size_t      chunk_size;
  size_t      chunk_pos;
  size_t      chunk_len;
} buffer_input_full_t;

cl_stream_t *stream_create_buffer_input(emacs_env *env, emacs_value buffer,
                                        ptrdiff_t start, ptrdiff_t end) {
  buffer_input_full_t *bi = calloc(1, sizeof(*bi));
  if (!bi) return NULL;

  bi->env = env;
  bi->buffer_ref = env->make_global_ref(env, buffer);
  bi->pos = start;
  bi->end = end;
  bi->chunk_size = BUFFER_STREAM_CHUNK;
  bi->chunk = malloc(bi->chunk_size);
  if (!bi->chunk) {
    env->free_global_ref(env, bi->buffer_ref);
    free(bi);
    return NULL;
  }
  bi->chunk_pos = 0;
  bi->chunk_len = 0;

  buffer_input_impl_t *bi_pub = malloc(sizeof(*bi_pub));
  if (!bi_pub) {
    env->free_global_ref(env, bi->buffer_ref);
    free(bi->chunk);
    free(bi);
    return NULL;
  }
  bi_pub->buffer = bi->buffer_ref;
  bi_pub->pos = start;
  bi_pub->end = end;
  bi_pub->owns_ref = true;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) {
    env->free_global_ref(env, bi->buffer_ref);
    free(bi->chunk);
    free(bi);
    free(bi_pub);
    return NULL;
  }

  s->ops = &buffer_input_ops;
  s->impl = bi;
  s->kind = CL_STREAM_KIND_BUFFER_INPUT;
  s->flags = CL_STREAM_FLAG_INPUT;
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

static ptrdiff_t fill_chunk(buffer_input_full_t *bi) {
  if (bi->pos >= bi->end) return 0;
  ptrdiff_t remaining = bi->end - bi->pos;
  ptrdiff_t to_read = remaining < (ptrdiff_t)bi->chunk_size
                      ? remaining : (ptrdiff_t)bi->chunk_size;

  emacs_env *env = bi->env;

  emacs_value args[2];
  args[0] = eint(env, bi->pos);
  args[1] = eint(env, bi->pos + to_read);
  emacs_value text = efuncall(env, "buffer-substring-no-properties", 2, args);

  ptrdiff_t copied = (ptrdiff_t)bi->chunk_size;
  if (!env->copy_string_contents(env, text, bi->chunk, &copied)) return 0;
  copied -= 1; /* exclude null terminator */

  bi->pos += copied;
  bi->chunk_len = (size_t)copied;
  bi->chunk_pos = 0;
  return copied;
}

static ptrdiff_t bis_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return 0;

  ptrdiff_t total = 0;
  unsigned char *dest = (unsigned char *)buf;

  while (total < n) {
    if (bi->chunk_pos >= bi->chunk_len) {
      if (fill_chunk(bi) <= 0) break;
    }
    size_t avail = bi->chunk_len - bi->chunk_pos;
    size_t want = (size_t)(n - total);
    size_t to_copy = avail < want ? avail : want;
    memcpy(dest + total, bi->chunk + bi->chunk_pos, to_copy);
    bi->chunk_pos += to_copy;
    total += (ptrdiff_t)to_copy;
  }
  return total;
}

static int bis_close(cl_stream_t *s) {
  if (!atomic_load(&s->open)) return 0;
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) {
    emacs_env *env = bi->env;
    env->free_global_ref(env, bi->buffer_ref);
    free(bi->chunk);
  }
  atomic_store(&s->open, false);
  return 0;
}

static void bis_destroy(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (bi) {
    free(bi->chunk);
    free(bi);
  }
}

static bool bis_listen(cl_stream_t *s) {
  buffer_input_full_t *bi = (buffer_input_full_t *)s->impl;
  if (!bi) return false;
  if (bi->chunk_pos < bi->chunk_len) return true;
  return bi->pos < bi->end;
}

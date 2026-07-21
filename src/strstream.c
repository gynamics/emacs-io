#include "stream.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ── string-input ops ────────────────────────────────────────────────── */

static ptrdiff_t sis_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       sis_close(cl_stream_t *s);
static bool      sis_listen(cl_stream_t *s);
static void      sis_destroy(cl_stream_t *s);
static bool      sis_direct(cl_stream_t *s, const void **addr, size_t *len);
static void      sis_advance(cl_stream_t *s, ptrdiff_t n);

static const cl_stream_ops_t string_input_ops = {
  .read     = sis_read,
  .write    = NULL,
  .close    = sis_close,
  .flush    = NULL,
  .finish   = NULL,
  .listen   = sis_listen,
  .destroy  = sis_destroy,
  .direct   = sis_direct,
  .advance  = sis_advance,
};

/* ── string-output ops ───────────────────────────────────────────────── */

static ptrdiff_t sos_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static int       sos_close(cl_stream_t *s);
static void      sos_destroy(cl_stream_t *s);
static ptrdiff_t sos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str);

static const cl_stream_ops_t string_output_ops = {
  .read         = NULL,
  .write        = sos_write,
  .close        = sos_close,
  .flush        = NULL,
  .finish       = NULL,
  .listen       = NULL,
  .destroy      = sos_destroy,
  .direct       = NULL,
  .advance      = NULL,
  .write_string = sos_write_string,
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

static bool sis_direct(cl_stream_t *s, const void **addr, size_t *len) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || si->pos >= si->len) return false;
  *addr = si->data + si->pos;
  *len = si->len - si->pos;
  return true;
}

static void sis_advance(cl_stream_t *s, ptrdiff_t n) {
  string_input_impl_t *si = (string_input_impl_t *)s->impl;
  if (!si || n <= 0) return;
  si->pos += (size_t)n;
  if (si->pos > si->len) si->pos = si->len;
}

/* ── string output: block-chain (no realloc copying) ────────────────────*/

cl_stream_t *stream_create_string_output(size_t hint_size) {
  str_output_impl_t *so = calloc(1, sizeof(*so));
  if (!so) return NULL;

  /* pre-allocate a single block if caller knows the expected size */
  if (hint_size > 0) {
    str_block_t *bk = malloc(sizeof(*bk) + hint_size + 1);  /* +1 for null */
    if (bk) {
      bk->next = NULL;
      bk->len  = 0;
      bk->cap  = hint_size;  /* user-visible capacity */
      so->head = bk;
      so->tail = bk;
    }
  }

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) {
    str_block_t *bk = so->head;
    while (bk) { str_block_t *n = bk->next; free(bk); bk = n; }
    free(so);
    return NULL;
  }

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
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return -1;

  /* If tail has room, write directly — zero old-data copying */
  if (so->tail && so->tail->cap - so->tail->len >= (size_t)n) {
    memcpy(so->tail->data + so->tail->len, buf, (size_t)n);
    so->tail->len += (size_t)n;
    so->total_len += (size_t)n;
    return n;
  }

  /* Allocate new block.  For large writes, match size exactly;
     otherwise use default block size. */
  size_t new_cap = STR_BLOCK_DEFAULT_CAP;
  if ((size_t)n > new_cap) new_cap = (size_t)n;

  str_block_t *bk = malloc(sizeof(*bk) + new_cap);
  if (!bk) return -1;
  bk->next = NULL;
  bk->len = (size_t)n;
  bk->cap = new_cap;
  memcpy(bk->data, buf, (size_t)n);

  if (!so->head) so->head = bk;
  else so->tail->next = bk;
  so->tail = bk;
  so->total_len += (size_t)n;

  return n;
}

static ptrdiff_t sos_write_string(emacs_env *env, cl_stream_t *s, emacs_value str) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return -1;

  /* query size */
  ptrdiff_t slen = 0;
  if (!env->copy_string_contents(env, str, NULL, &slen)) return -1;
  if (slen <= 1) return 0;

  size_t n = (size_t)(slen - 1);

  /* if tail block has room, write directly into it */
  if (so->tail && so->tail->cap - so->tail->len >= n) {
    ptrdiff_t copied = slen;
    if (!env->copy_string_contents(env, str, so->tail->data + so->tail->len, &copied)) return -1;
    so->tail->len += n;
    so->total_len += n;
    return (ptrdiff_t)n;
  }

  /* allocate new block */
  str_block_t *bk = malloc(sizeof(*bk) + n + 1);
  if (!bk) return -1;
  bk->next = NULL;
  bk->len = n;
  bk->cap = n;

  ptrdiff_t copied = slen;
  if (!env->copy_string_contents(env, str, bk->data, &copied)) {
    free(bk);
    return -1;
  }

  if (!so->head) so->head = bk;
  else so->tail->next = bk;
  so->tail = bk;
  so->total_len += n;

  return (ptrdiff_t)n;
}

static int sos_close(cl_stream_t *s) {
  if (!atomic_load(&s->open)) return 0;
  atomic_store(&s->open, false);
  return 0;
}

static void sos_destroy(cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return;
  str_block_t *bk = so->head;
  while (bk) {
    str_block_t *next = bk->next;
    free(bk);
    bk = next;
  }
  free(so);
}

/* ── get accumulated string from block-chain output stream ───────────── */

emacs_value str_output_to_string(emacs_env *env, cl_stream_t *s) {
  str_output_impl_t *so = (str_output_impl_t *)s->impl;
  if (!so) return eintern(env, "nil");

  if (!so->head) return env->make_string(env, "", 0);

  /* Single block — return directly */
  if (so->head == so->tail)
    return env->make_string(env, so->head->data, (ptrdiff_t)so->head->len);

  /* Multiple blocks — use Emacs concat (uses Emacs' own allocator) */
  int nblocks = 0;
  for (str_block_t *bk = so->head; bk; bk = bk->next) nblocks++;

  emacs_value *elts = malloc((size_t)nblocks * sizeof(emacs_value));
  if (!elts) return eintern(env, "nil");
  str_block_t *bk = so->head;
  for (int i = 0; i < nblocks; i++, bk = bk->next) {
    if (!bk) break;
    elts[i] = env->make_string(env, bk->data, (ptrdiff_t)bk->len);
  }

  emacs_value concat_sym = eintern(env, "concat");
  emacs_value result = env->funcall(env, concat_sym, nblocks, elts);
  free(elts);
  return result;
}

/* ── mmap-backed input stream (regular files only) ────────────────────── */

static ptrdiff_t mmap_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       mmap_close(cl_stream_t *s);
static bool      mmap_listen(cl_stream_t *s);
static void      mmap_destroy(cl_stream_t *s);
static bool      mmap_direct(cl_stream_t *s, const void **addr, size_t *len);
static void      mmap_advance(cl_stream_t *s, ptrdiff_t n);

static const cl_stream_ops_t mmap_stream_ops = {
  .read     = mmap_read,
  .write    = NULL,
  .close    = mmap_close,
  .flush    = NULL,
  .finish   = NULL,
  .listen   = mmap_listen,
  .destroy  = mmap_destroy,
  .direct   = mmap_direct,
  .advance  = mmap_advance,
};

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

cl_stream_t *stream_create_mmap(int fd, bool owns_fd) {
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0) return NULL;

  void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ,
                    MAP_PRIVATE, fd, 0);
  if (addr == MAP_FAILED) return NULL;

  mmap_stream_impl_t *mi = calloc(1, sizeof(*mi));
  if (!mi) { munmap(addr, (size_t)st.st_size); return NULL; }

  mi->addr = addr;
  mi->size = (size_t)st.st_size;
  mi->pos  = 0;
  mi->fd   = fd;
  mi->owns_fd = owns_fd;

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) { munmap(addr, (size_t)st.st_size); free(mi); return NULL; }

  s->ops = &mmap_stream_ops;
  s->impl = mi;
  s->kind = CL_STREAM_KIND_MMAP;
  s->flags = CL_STREAM_FLAG_INPUT;
  s->file_mode = st.st_mode;  /* already have from fstat above */
  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  return s;
}

static ptrdiff_t mmap_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (!mi || mi->pos >= mi->size) return 0;

  size_t remaining = mi->size - mi->pos;
  size_t to_copy = (size_t)n < remaining ? (size_t)n : remaining;
  memcpy(buf, (const char *)mi->addr + mi->pos, to_copy);
  mi->pos += to_copy;
  return (ptrdiff_t)to_copy;
}

static int mmap_close(cl_stream_t *s) {
  if (!atomic_load(&s->open)) return 0;
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (mi) {
    munmap(mi->addr, mi->size);
    if (mi->owns_fd && mi->fd >= 0) close(mi->fd);
  }
  atomic_store(&s->open, false);
  return 0;
}

static void mmap_destroy(cl_stream_t *s) {
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (mi) free(mi);
}

static bool mmap_listen(cl_stream_t *s) {
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  return mi != NULL && mi->pos < mi->size;
}

static bool mmap_direct(cl_stream_t *s, const void **addr, size_t *len) {
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (!mi || mi->pos >= mi->size) return false;
  *addr = (const char *)mi->addr + mi->pos;
  *len = mi->size - mi->pos;
  return true;
}

static void mmap_advance(cl_stream_t *s, ptrdiff_t n) {
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (!mi || n <= 0) return;
  mi->pos += (size_t)n;
  if (mi->pos > mi->size) mi->pos = mi->size;
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

#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── mmap-backed input stream (regular files only) ────────────────────── */

static ptrdiff_t mmap_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static int       mmap_close(cl_stream_t *s);
static bool      mmap_listen(cl_stream_t *s);
static void      mmap_destroy(cl_stream_t *s);
static bool      mmap_direct(cl_stream_t *s, const void **addr, size_t *len);
static void      mmap_advance(cl_stream_t *s, ptrdiff_t n);
static ptrdiff_t mmap_file_position(cl_stream_t *s);
static bool      mmap_set_file_position(cl_stream_t *s, ptrdiff_t pos);

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
  .file_position = mmap_file_position,
  .set_file_position = mmap_set_file_position,
};

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

static ptrdiff_t mmap_file_position(cl_stream_t *s) {
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (!mi) return -1;
  return (ptrdiff_t)mi->pos;
}

static bool mmap_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  mmap_stream_impl_t *mi = (mmap_stream_impl_t *)s->impl;
  if (!mi || pos < 0 || (size_t)pos > mi->size) return false;
  mi->pos = (size_t)pos;
  return true;
}


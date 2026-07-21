#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>

/* ── internal fd-stream ops ──────────────────────────────────────────── */

static ptrdiff_t fds_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static ptrdiff_t fds_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static int       fds_close(cl_stream_t *s);
static int       fds_flush(cl_stream_t *s);
static int       fds_finish(cl_stream_t *s);
static bool      fds_listen(cl_stream_t *s);
static void      fds_destroy(cl_stream_t *s);

static const cl_stream_ops_t fd_stream_ops = {
  .read    = fds_read,
  .write   = fds_write,
  .close   = fds_close,
  .flush   = fds_flush,
  .finish  = fds_finish,
  .listen  = fds_listen,
  .destroy = fds_destroy,
};

/* ── create / destroy ────────────────────────────────────────────────── */

cl_stream_t *stream_create_fd(int fd, bool owns_fd, unsigned flags) {
  fd_stream_impl_t *fi = calloc(1, sizeof(*fi));
  if (!fi) return NULL;

  fi->fd = fd;
  fi->owns_fd = owns_fd;
  fi->rsz = FD_STREAM_RBUF_DEFAULT;
  fi->rbuf = malloc(fi->rsz);
  fi->rpos = 0;
  fi->rlen = 0;
  fi->wsz = FD_STREAM_WBUF_DEFAULT;
  fi->wbuf = malloc(fi->wsz);
  fi->wlen = 0;

  if (!fi->rbuf || !fi->wbuf) {
    free(fi->rbuf);
    free(fi->wbuf);
    free(fi);
    return NULL;
  }

  cl_stream_t *s = calloc(1, sizeof(*s));
  if (!s) {
    free(fi->rbuf);
    free(fi->wbuf);
    free(fi);
    return NULL;
  }

  s->ops = &fd_stream_ops;
  s->impl = fi;
  s->kind = CL_STREAM_KIND_FD;
  s->flags = flags;

  /* detect file type via standard Linux macros */
  {
    struct stat st;
    if (fstat(fd, &st) == 0)
      s->file_mode = st.st_mode;
    if (isatty(fd))
      s->flags |= CL_STREAM_FLAG_INTERACTIVE;
  }

  atomic_init(&s->open, true);
  pthread_mutex_init(&s->mutex, NULL);

  /* apply non-blocking mode to fd */
  if (flags & CL_STREAM_FLAG_NONBLOCK) {
    int fdflags = fcntl(fd, F_GETFL, 0);
    if (fdflags >= 0)
      fcntl(fd, F_SETFL, fdflags | O_NONBLOCK);
  }

  return s;
}

void stream_free(cl_stream_t *s) {
  if (!s) return;

  pthread_mutex_destroy(&s->mutex);

  if (s->ops && s->ops->destroy)
    s->ops->destroy(s);

  free(s);
}

/* ── read ─────────────────────────────────────────────────────────────── */

static ptrdiff_t fds_read(cl_stream_t *s, void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return -1;
  if (!(s->flags & CL_STREAM_FLAG_INPUT)) return -1;

  ptrdiff_t total = 0;
  unsigned char *dest = (unsigned char *)buf;

  /* first, drain existing read buffer */
  while (fi->rpos < fi->rlen && total < n) {
    dest[total++] = fi->rbuf[fi->rpos++];
  }
  if (fi->rpos >= fi->rlen) {
    fi->rpos = 0;
    fi->rlen = 0;
  }
  if (total >= n) return total;

  /* if remaining request is large, bypass buffer */
  if (n - total >= (ptrdiff_t)fi->rsz) {
    ssize_t r = read(fi->fd, dest + total, (size_t)(n - total));
    if (r < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return total > 0 ? total : 0;
      return -1;
    }
    if (r == 0) return total > 0 ? total : 0; /* EOF */
    total += r;
    return total;
  }

  /* fill buffer and serve from it */
  ssize_t r = read(fi->fd, fi->rbuf, fi->rsz);
  if (r < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return total > 0 ? total : 0;
    return -1;
  }
  if (r == 0) return total > 0 ? total : 0; /* EOF */
  fi->rlen = (size_t)r;
  fi->rpos = 0;

  while (fi->rpos < fi->rlen && total < n) {
    dest[total++] = fi->rbuf[fi->rpos++];
  }
  return total;
}

/* ── write ────────────────────────────────────────────────────────────── */

static ptrdiff_t fds_write(cl_stream_t *s, const void *buf, ptrdiff_t n) {
  if (!s || !buf || n <= 0) return 0;
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return -1;
  if (!(s->flags & CL_STREAM_FLAG_OUTPUT)) return -1;

  const unsigned char *src = (const unsigned char *)buf;

  /* if data fits in write buffer, buffer it */
  if (fi->wlen + (size_t)n <= fi->wsz) {
    memcpy(fi->wbuf + fi->wlen, src, (size_t)n);
    fi->wlen += (size_t)n;
    return n;
  }

  /* flush existing buffer first */
  if (fi->wlen > 0) {
    fds_flush(s);
    if (fi->wlen > 0) return -1; /* flush failed */
  }

  /* if larger than buffer, write directly */
  if ((size_t)n >= fi->wsz) {
    ssize_t w = write(fi->fd, src, (size_t)n);
    if (w < 0) return -1;
    return (ptrdiff_t)w;
  }

  /* buffer it */
  memcpy(fi->wbuf, src, (size_t)n);
  fi->wlen = (size_t)n;
  return n;
}

/* ── close ────────────────────────────────────────────────────────────── */

static int fds_close(cl_stream_t *s) {
  if (!atomic_load(&s->open)) return 0;
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi) return -1;

  /* flush write buffer */
  if (fi->wlen > 0 && (s->flags & CL_STREAM_FLAG_OUTPUT))
    fds_flush(s);

  if (fi->owns_fd && fi->fd >= 0) {
    close(fi->fd);
    fi->fd = -1;
  }

  atomic_store(&s->open, false);
  return 0;
}

/* ── flush ────────────────────────────────────────────────────────────── */

static int fds_flush(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->wlen == 0) return 0;

  size_t offset = 0;
  while (offset < fi->wlen) {
    ssize_t w = write(fi->fd, fi->wbuf + offset, fi->wlen - offset);
    if (w < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return -1;
    }
    offset += (size_t)w;
  }
  /* shift remaining data to front */
  if (offset > 0 && offset < fi->wlen)
    memmove(fi->wbuf, fi->wbuf + offset, fi->wlen - offset);
  fi->wlen -= offset;
  return 0;
}

/* ── finish (fsync) ───────────────────────────────────────────────────── */

static int fds_finish(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return -1;
  int rc = fds_flush(s);
  if (rc < 0) return rc;
  if (fsync(fi->fd) < 0 && errno != EINVAL && errno != EROFS)
    return -1;
  return 0;
}

/* ── listen ───────────────────────────────────────────────────────────── */

static bool fds_listen(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return false;
  if (!(s->flags & CL_STREAM_FLAG_INPUT)) return false;

  /* data in read buffer */
  if (fi->rpos < fi->rlen) return true;

  /* try a non-blocking peek */
  int fdflags = fcntl(fi->fd, F_GETFL, 0);
  if (fdflags < 0) return false;
  bool was_nonblock = (fdflags & O_NONBLOCK) != 0;
  if (!was_nonblock)
    fcntl(fi->fd, F_SETFL, fdflags | O_NONBLOCK);

  unsigned char peek;
  ssize_t r = read(fi->fd, &peek, 1);

  if (!was_nonblock)
    fcntl(fi->fd, F_SETFL, fdflags);

  if (r == 1) {
    /* put the byte into the read buffer */
    if (fi->rlen < fi->rsz) {
      fi->rbuf[fi->rlen++] = peek;
    }
    return true;
  }
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return false;
  return false;
}

/* ── destroy ─────────────────────────────────────────────────────────────︎ */

static void fds_destroy(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (fi) {
    free(fi->rbuf);
    free(fi->wbuf);
    free(fi);
  }
}

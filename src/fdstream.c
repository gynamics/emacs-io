#define _POSIX_C_SOURCE 200809L
#include "stream.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/uio.h>

/* ── internal fd-stream ops ──────────────────────────────────────────── */

static ptrdiff_t fds_read(cl_stream_t *s, void *buf, ptrdiff_t n);
static ptrdiff_t fds_write(cl_stream_t *s, const void *buf, ptrdiff_t n);
static int       fds_close(cl_stream_t *s);
static int       fds_flush(cl_stream_t *s);
static int       fds_finish(cl_stream_t *s);
static bool      fds_listen(cl_stream_t *s);
static void      fds_destroy(cl_stream_t *s);
static void      fds_clear_input(cl_stream_t *s);
static void      fds_clear_output(cl_stream_t *s);
static bool      fds_direct(cl_stream_t *s, const void **addr, size_t *len);
static void      fds_advance(cl_stream_t *s, ptrdiff_t n);
static ptrdiff_t fds_file_position(cl_stream_t *s);
static bool      fds_set_file_position(cl_stream_t *s, ptrdiff_t pos);
static ptrdiff_t fds_file_length(cl_stream_t *s);
static bool      fds_truncate(cl_stream_t *s, ptrdiff_t len);

static const cl_stream_ops_t fd_stream_ops = {
  .read         = fds_read,
  .write        = fds_write,
  .close        = fds_close,
  .flush        = fds_flush,
  .finish       = fds_finish,
  .listen       = fds_listen,
  .destroy      = fds_destroy,
  .direct       = fds_direct,
  .advance      = fds_advance,
  .clear_input  = fds_clear_input,
  .clear_output = fds_clear_output,
  .file_position = fds_file_position,
  .set_file_position = fds_set_file_position,
  .file_length    = fds_file_length,
  .truncate    = fds_truncate,
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

  cl_stream_t *s = stream_alloc(&fd_stream_ops, fi, CL_STREAM_KIND_FD, flags);
  if (!s) {
    free(fi->rbuf);
    free(fi->wbuf);
    free(fi);
    return NULL;
  }

  /* detect file type via standard Linux macros */
  {
    struct stat st;
    if (fstat(fd, &st) == 0)
      s->file_mode = st.st_mode;
    if (isatty(fd))
      s->flags |= CL_STREAM_FLAG_INTERACTIVE;
  }

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

  stream_registry_remove(s);
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

/* ── zero-copy window ─────────────────────────────────────────────────── */

/* Expose the buffered input without consuming it (refilling if empty) so a
   reader can scan for a line/object boundary and then advance exactly. */
static bool fds_direct(cl_stream_t *s, const void **addr, size_t *len) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return false;
  if (!(s->flags & CL_STREAM_FLAG_INPUT)) return false;

  if (fi->rpos >= fi->rlen) {
    ssize_t r = read(fi->fd, fi->rbuf, fi->rsz);
    if (r <= 0) return false;             /* EOF or EAGAIN */
    fi->rlen = (size_t)r;
    fi->rpos = 0;
  }
  *addr = fi->rbuf + fi->rpos;
  *len = fi->rlen - fi->rpos;
  return true;
}

static void fds_advance(cl_stream_t *s, ptrdiff_t n) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || n <= 0) return;
  if ((size_t)n >= fi->rlen - fi->rpos) fi->rpos = fi->rlen;
  else fi->rpos += (size_t)n;
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

  struct pollfd pfd;
  pfd.fd = fi->fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  int rc = poll(&pfd, 1, 0);
  if (rc <= 0) return false;

  /* peek one byte into the read buffer */
  ssize_t r = read(fi->fd, &fi->rbuf[fi->rlen], 1);
  if (r == 1) {
    fi->rlen++;
    return true;
  }
  return false;
}

/* ── destroy ─────────────────────────────────────────────────────────────︎ */

static void fds_destroy(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (fi) {
    free(fi->rbuf);
    free(fi->wbuf);
    free(fi->path);
    free(fi);
  }
}

static void fds_clear_input(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (fi) { fi->rpos = 0; fi->rlen = 0; }
}

static void fds_clear_output(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (fi) fi->wlen = 0;
}

static ptrdiff_t fds_file_position(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return -1;
  /* flush pending writes so lseek reflects the true file offset */
  if (fi->wlen > 0) fds_flush(s);
  off_t pos = lseek(fi->fd, 0, SEEK_CUR);
  if (pos < 0) return -1;
  /* adjust for unread bytes in the read buffer */
  if (fi->rpos < fi->rlen)
    pos -= (off_t)(fi->rlen - fi->rpos);
  return (ptrdiff_t)pos;
}

static bool fds_set_file_position(cl_stream_t *s, ptrdiff_t pos) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0 || pos < 0) return false;
  if (fi->wlen > 0) fds_flush(s);
  /* Seek first: on a non-seekable fd (pipe/socket/tty) this fails and we
     must leave the read buffer intact. */
  if (lseek(fi->fd, (off_t)pos, SEEK_SET) < 0) return false;
  fi->rpos = 0; fi->rlen = 0;
  return true;
}

static ptrdiff_t fds_file_length(cl_stream_t *s) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return -1;
  if (fi->wlen > 0) fds_flush(s);
  off_t cur = lseek(fi->fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  off_t end = lseek(fi->fd, 0, SEEK_END);
  if (end < 0) { lseek(fi->fd, cur, SEEK_SET); return -1; }
  lseek(fi->fd, cur, SEEK_SET);
  return (ptrdiff_t)end;
}

static bool fds_truncate(cl_stream_t *s, ptrdiff_t len) {
  fd_stream_impl_t *fi = (fd_stream_impl_t *)s->impl;
  if (!fi || fi->fd < 0) return false;
  /* flush write buffer before truncating */
  if (fi->wlen > 0) fds_flush(s);
  return ftruncate(fi->fd, (off_t)len) == 0;
}

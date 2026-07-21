# emacs-io

CL-style stream I/O as a dynamic module for GNU Emacs 31.

```elisp
(module-load "emacs-io.so")
```

## Why

Emacs buffers are the universal I/O medium — and the bottleneck.  Every
`insert` triggers modification hooks, maintains undo history, repositions
markers and overlays, and forces gap-buffer moves.  A 100 MB log file loaded
into a temp buffer allocates 100 MB of gap-buffer, then garbage-collects it.

Streams bypass all of that.  Raw `read(2)` / `write(2)` through userspace
ring buffers, zero copy within string streams, and no hook machinery.

## Quick start

```
$ make             # → emacs-io.so
$ make verify      # confirm module loads in Emacs
$ make test        # 77 unit tests
$ make bench       # 12 performance benchmarks
```

`EMACS_IO_BENCH_FULL=1 make bench` enables the 10 MB-scale tests.

`emacs-module.h` is not in the repo — it comes from Emacs 31 dev headers.

## Stream types

| Constructor | Backend | Direction |
|---|---|---|
| `cl-open` | POSIX `open(2)`, 8 KB ring buffers, `O_NONBLOCK` support | any |
| `cl-make-string-input-stream` | C `const char*` with position cursor | input |
| `cl-make-string-output-stream` | Block-chain growable accumulator | output |
| `cl-make-buffer-input-stream` | Emacs buffer via `buffer-substring-no-properties`, 64 KB chunks | input |

Standard streams (`*standard-input*`, `*standard-output*`, `*error-output*`,
`*trace-output*`, `*debug-io*`, `*query-io*`, `*terminal-io*`) are
initialized at module load, mapped to file descriptors 0, 1, and 2.

## API

### Stream lifecycle
`cl-open` `cl-close` `cl-open-stream-p` `cl-input-stream-p` `cl-output-stream-p` `cl-stream-element-type` `cl-file-regular-p` `cl-file-tty-p`

### String streams
`cl-make-string-input-stream` `cl-make-string-output-stream` `cl-get-output-stream-string`

### Character I/O
`cl-read-char` `cl-read-line` `cl-read-byte` `cl-peek-char` `cl-unread-char` `cl-listen`
`cl-write-char` `cl-write-string` `cl-write-line` `cl-write-byte`
`cl-terpri` `cl-fresh-line`

### Bulk I/O
`cl-read-sequence` `cl-write-sequence` `cl-read-string` `cl-read-chunk`

### Stream control
`cl-finish-output` (fsync) `cl-force-output` (fflush) `cl-clear-output` `cl-clear-input`

### Lisp reader
`cl-read` `cl-read-from-string`

### Lisp printer
`cl-write` `cl-prin1` `cl-princ` `cl-print` `cl-prin1-to-string` `cl-princ-to-string`

## Performance

All numbers from Emacs 31.0.90, Linux x86-64, GCC 14 `-O2`.  Every test
compares stream I/O against the most efficient Emacs native equivalent.

| Test | Stream | Buffer | Speedup |
|---|---|---|---|
| `cl-read-sequence` 100 K elements | 2.3 ms | 33.4 ms | **14.7×** |
| `cl-write-sequence` 100 K elements | 2.4 ms | 31.0 ms | **12.9×** |
| File read/write 10 MB | 4.5 ms | 42.7 ms | **9.6×** |
| File fd write 10 MB | 5.0 ms | 32.0 ms | **6.4×** |
| Log grep 10 K lines | 1.6 ms | 4.8 ms | **3.0×** |
| Comint incremental 10 K lines | 1.5 ms | 4.6 ms | **3.0×** |
| Chunk read 1 MB | 0.3 ms | 0.6 ms | **2.4×** |
| Accumulate + write to disk 10 MB | 17.4 ms | 37.6 ms | **2.2×** |
| Per-char write 100 K | 18.1 ms | 34.2 ms | **1.9×** |
| Line read 10 K lines | 2.1 ms | 3.3 ms | **1.6×** |
| Bulk read 10 MB | 6.6 ms | 9.9 ms | **1.5×** |
| Pipe read 10 MB | 5.9 ms | 8.1 ms | **1.4×** |

Sequence I/O wins by **15×** by eliminating per-element C↔Lisp crossings:
a single C `memcpy` of 100 KB replaces 100,000 iterations of `(aset vec i
(char-after ...))`.  File I/O wins by **10×** via raw `write(2)` bypassing
`write-region` encoding and mode hooks.  Line processing uses a 4 KB
bulk-chunk scanner with C-level newline detection instead of per-byte
dispatch.

## Architecture

```
src/stream.h     — cl_stream struct, vtable, impl structs, public API
src/stream.c     — dispatch layer: lock, 256-slot pushback ring, call vtable
src/fdstream.c   — POSIX fd I/O with 8 KB userspace ring buffers
src/mmapstream.c — mmap-backed input for regular files (zero-copy reads)
src/strstream.c  — string-input (cursor), string-output (block-chain)
src/bufstream.c  — buffer input via buffer-substring-no-properties, 64 KB chunks
src/reader.c     — cl-read-char, cl-read-line (CRLF-aware), bulk chunk read
src/printer.c    — cl-write-*, terpri, fresh-line, flush/force/finish/clear
src/fileio.c     — cl-open, cl-close, string/buffer constructors, predicates
src/lispio.c     — cl-read (→ read-from-string), cl-prin1/princ/print
src/stdstream.c  — standard stream variable init
src/util.c/h     — emacs_env wrappers, defun, kw_eq, strbuf, error signaling
src/emacs-io.c   — emacs_module_init, 40 defun registrations
```

Every I/O operation acquires a per-stream `pthread_mutex_t`.  Streams are
flagged `O_NONBLOCK` and queried with `cl-listen` — ready for `select`/`poll`
event loops.  `cl-read-chunk` and `cl-read-string` use `stream_direct` for
zero-copy reads from mmap and string-input streams.

## Notes

- **Windows unsupported**: Targets Linux/BSD/macOS.  Uses `memfd_create`, `O_NONBLOCK`, `fsync`, `mmap`, `pthread`.

- **ERT byte-compiler quirk** (Emacs 31.0.90): `cl-fresh-line` and `cl-prin1` trigger `(invalid-function #<user-ptr ...>)` when called with a stream argument inside an `ert-deftest` body.  The functions work correctly in normal evaluation.  Tests for these use `*-to-string` variants.

## License

GPL-3.0+

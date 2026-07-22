#include "emacs-module.h"
#include "stream.h"
#include "util.h"
#include <string.h>

int plugin_is_GPL_compatible;

/* ── forward declarations for all registered functions ────────────────── */

/* reader.c */
extern emacs_value Fread_char(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data);
extern emacs_value Fread_line(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data);
extern emacs_value Fread_byte(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);
extern emacs_value Fread_char_no_hang(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data);
extern emacs_value Fread_preserving_whitespace(emacs_env *env,
                                                ptrdiff_t nargs,
                                                emacs_value *args,
                                                void *data);
extern emacs_value Fread_delimited_list(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value *args, void *data);
extern emacs_value Fpeek_char(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data);
extern emacs_value Funread_char(emacs_env *env, ptrdiff_t nargs,
                                 emacs_value *args, void *data);
extern emacs_value Flisten(emacs_env *env, ptrdiff_t nargs,
                            emacs_value *args, void *data);
extern emacs_value Fread_sequence(emacs_env *env, ptrdiff_t nargs,
                                   emacs_value *args, void *data);
extern emacs_value Fread_string(emacs_env *env, ptrdiff_t nargs,
                                 emacs_value *args, void *data);
extern emacs_value Fread_chunk(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);

/* printer.c */
extern emacs_value Fwrite_char(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);
extern emacs_value Fwrite_string(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data);
extern emacs_value Fwrite_line(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);
extern emacs_value Fwrite_byte(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);
extern emacs_value Fterpri(emacs_env *env, ptrdiff_t nargs,
                            emacs_value *args, void *data);
extern emacs_value Ffresh_line(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);
extern emacs_value Ffinish_output(emacs_env *env, ptrdiff_t nargs,
                                   emacs_value *args, void *data);
extern emacs_value Fforce_output(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data);
extern emacs_value Fclear_output(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data);
extern emacs_value Fclear_input(emacs_env *env, ptrdiff_t nargs,
                                 emacs_value *args, void *data);
extern emacs_value Fwrite_sequence(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data);

/* fileio.c */
extern emacs_value Fopen(emacs_env *env, ptrdiff_t nargs,
                          emacs_value *args, void *data);
extern emacs_value Fclose(emacs_env *env, ptrdiff_t nargs,
                           emacs_value *args, void *data);
extern emacs_value Fmake_string_input(emacs_env *env, ptrdiff_t nargs,
                                       emacs_value *args, void *data);
extern emacs_value Fmake_string_output(emacs_env *env, ptrdiff_t nargs,
                                        emacs_value *args, void *data);
extern emacs_value Fget_output_stream_string(emacs_env *env, ptrdiff_t nargs,
                                              emacs_value *args, void *data);
  extern emacs_value Fstreamp(emacs_env *env, ptrdiff_t nargs,
                             emacs_value *args, void *data);
extern emacs_value Fopen_stream_p(emacs_env *env, ptrdiff_t nargs,
                                   emacs_value *args, void *data);
extern emacs_value Finput_stream_p(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data);
extern emacs_value Foutput_stream_p(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args, void *data);
extern emacs_value Fstream_element_type(emacs_env *env, ptrdiff_t nargs,
                                         emacs_value *args, void *data);
extern emacs_value Fmake_buffer_input_stream(emacs_env *env, ptrdiff_t nargs,
                                               emacs_value *args, void *data);
extern emacs_value Ffile_regular_p(emacs_env *env, ptrdiff_t nargs,
                                    emacs_value *args, void *data);
extern emacs_value Ffile_tty_p(emacs_env *env, ptrdiff_t nargs,
                                emacs_value *args, void *data);
extern emacs_value Ffile_position(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data);

  extern emacs_value Ffile_length(emacs_env *env, ptrdiff_t nargs,
                                  emacs_value *args, void *data);

  extern emacs_value Fstream_external_format(emacs_env *env, ptrdiff_t nargs,
                                            emacs_value *args, void *data);
extern emacs_value Finteractive_stream_p(emacs_env *env, ptrdiff_t nargs,
                                          emacs_value *args, void *data);

/* lispio.c */
extern emacs_value Fread(emacs_env *env, ptrdiff_t nargs,
                          emacs_value *args, void *data);
extern emacs_value Fread_from_string(emacs_env *env, ptrdiff_t nargs,
                                      emacs_value *args, void *data);
extern emacs_value Fwrite(emacs_env *env, ptrdiff_t nargs,
                           emacs_value *args, void *data);
extern emacs_value Fprin1(emacs_env *env, ptrdiff_t nargs,
                           emacs_value *args, void *data);
extern emacs_value Fprinc(emacs_env *env, ptrdiff_t nargs,
                           emacs_value *args, void *data);
extern emacs_value Fprint(emacs_env *env, ptrdiff_t nargs,
                           emacs_value *args, void *data);
extern emacs_value Fprin1_to_string(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args, void *data);
extern emacs_value Fprinc_to_string(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args, void *data);
extern emacs_value Fwrite_to_string(emacs_env *env, ptrdiff_t nargs,
                                     emacs_value *args, void *data);
extern emacs_value Fpprint(emacs_env *env, ptrdiff_t nargs,
                            emacs_value *args, void *data);

/* stdstream.c */
extern void init_standard_streams(emacs_env *env);

/* ── module entry point ───────────────────────────────────────────────── */

int emacs_module_init(struct emacs_runtime *runtime) {
  emacs_env *env = runtime->get_environment(runtime);

  /* ensure we have at least emacs_env_31 */
  if ((size_t)env->size < sizeof(struct emacs_env_31)) return 1;

  syms_init(env);

  cl_rt = runtime;

  /* ── reader ──────────────────────────────────────────────────────── */
  defun(env, "cl-read-char", Fread_char, 0, 4,
        "Read one character from STREAM and return it.\n"
        "\n"
        "STREAM defaults to `*standard-input*'.\n"
        "If EOF-ERROR-P is nil and end of file is reached, return EOF-VALUE\n"
        "(which defaults to nil).\n"
        "If EOF-ERROR-P is non-nil (the default), signal an error on EOF.\n"
        "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
        "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);

  defun(env, "cl-read-line", Fread_line, 0, 4,
        "Read a line of text from STREAM and return it as a list.\n"
        "\n"
        "Returns a list (LINE . MISSING-NEWLINE-P) where MISSING-NEWLINE-P\n"
        "is non-nil if the line was terminated by end-of-file rather than\n"
        "a newline.  The returned line does not include the newline.\n"
        "STREAM defaults to `*standard-input*'.\n"
        "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
        "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);

  defun(env, "cl-read-byte", Fread_byte, 1, 4,
        "Read one byte from STREAM and return it as an integer.\n"
        "\n"
        "If EOF-ERROR-P is nil and end of file is reached, return EOF-VALUE.\n"
        "If EOF-ERROR-P is non-nil (the default), signal an error on EOF.\n"
        "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
        "\n(fn STREAM &optional EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);

  defun(env, "cl-read-char-no-hang", Fread_char_no_hang, 0, 3,
        "Read a character from STREAM, or return nil if none is available.\n"
        "\n"
        "Unlike `cl-read-char', this function never blocks.\n"
        "If no character is immediately available, it returns nil.\n"
        "STREAM defaults to `*standard-input*'.\n"
        "EOF-ERROR-P and EOF-VALUE are accepted for compatibility.\n"
        "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE)", NULL);

  defun(env, "cl-peek-char", Fpeek_char, 0, 4,
        "Return the next character from STREAM without removing it.\n"
        "\n"
        "PEEK-TYPE controls which character is returned:\n"
        "  nil  — return the next character (the default)\n"
        "  t    — skip whitespace, then return the next character\n"
        "  char — skip characters equal to char, then return the next\n"
        "STREAM defaults to `*standard-input*'.\n"
        "\n(fn &optional PEEK-TYPE STREAM EOF-ERROR-P EOF-VALUE)", NULL);

  defun(env, "cl-unread-char", Funread_char, 1, 2,
        "Return CHARACTER to STREAM so it will be read again.\n"
        "\n"
        "CHARACTER must be the most recent character read from STREAM.\n"
        "STREAM defaults to `*standard-input*'.\n"
        "The pushback buffer holds up to 256 characters.\n"
        "\n(fn CHARACTER &optional STREAM)", NULL);

  defun(env, "cl-listen", Flisten, 0, 1,
        "Return non-nil if a character is available on STREAM.\n"
        "\n"
        "This checks whether the next call to `cl-read-char' would\n"
        "return immediately without blocking, including pushback\n"
        "characters and buffered data.\n"
        "STREAM defaults to `*standard-input*'.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-read-sequence", Fread_sequence, 2, 6,
        "Destructively modify SEQUENCE by replacing its elements\n"
        "with elements read from STREAM.\n"
        "\n"
        "SEQUENCE must be a vector or list.  Reading stops when the sequence\n"
        "is full or end of file is reached.\n"
        "Keywords :START and :END delimit the affected subsequence.\n"
        "Returns the index of the first element not updated.\n"
        "\n(fn SEQUENCE STREAM &key START END)", NULL);

  defun(env, "cl-read-string", Fread_string, 2, 5,
        "Read up to N bytes from STREAM and return them as a unibyte string.\n"
        "\n"
        "Fewer bytes may be returned if end of file is reached.\n"
        "If EOF-ERROR-P is nil and end of file is reached, return EOF-VALUE.\n"
        "Use this for bulk reads; it avoids per-element overhead of\n"
        "`cl-read-sequence' for large data.\n"
         "\n(fn N STREAM &optional EOF-ERROR-P EOF-VALUE)", NULL);

  defun(env, "cl-read-chunk", Fread_chunk, 1, 3,
        "Read up to MAX-BYTES (default 65536) from STREAM.\n"
        "\n"
        "Returns a cons (CHUNK . EOF-P) where CHUNK is a unibyte string\n"
        "and EOF-P is non-nil if end of file was reached.\n"
        "Use this for incremental bulk reads; it avoids the per-byte\n"
        "overhead of `cl-listen' + `cl-read-char' loops.\n"
        "\n(fn STREAM &optional MAX-BYTES)", NULL);

  /* ── printer ─────────────────────────────────────────────────────── */
  defun(env, "cl-write-char", Fwrite_char, 1, 2,
        "Write CHARACTER to STREAM.\n"
        "\n"
        "STREAM defaults to `*standard-output*'.\n"
        "Returns CHARACTER.\n"
        "\n(fn CHARACTER &optional STREAM)", NULL);

  defun(env, "cl-write-string", Fwrite_string, 1, 6,
        "Write STRING to STREAM.\n"
        "\n"
        "Keywords :START and :END delimit the substring to write.\n"
        "STREAM defaults to `*standard-output*'.\n"
        "Returns STRING.\n"
        "\n(fn STRING &optional STREAM &key START END)", NULL);

  defun(env, "cl-write-line", Fwrite_line, 1, 5,
        "Write STRING followed by a newline to STREAM.\n"
        "\n"
        "Equivalent to (progn (cl-write-string STRING STREAM) (cl-terpri STREAM)).\n"
        "Keywords :START and :END delimit the substring to write.\n"
        "Returns STRING.\n"
        "\n(fn STRING &optional STREAM &key START END)", NULL);

  defun(env, "cl-write-byte", Fwrite_byte, 2, 2,
        "Write BYTE (an integer 0–255) to STREAM.\n"
        "\n"
        "Returns BYTE.\n"
        "\n(fn BYTE STREAM)", NULL);

  defun(env, "cl-terpri", Fterpri, 0, 1,
        "Output a newline character to STREAM.\n"
        "\n"
        "STREAM defaults to `*standard-output*'.\n"
        "Returns nil.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-fresh-line", Ffresh_line, 0, 1,
        "Output a newline to STREAM if not at the start of a line.\n"
        "\n"
        "If it is uncertain whether output is at the start of a line,\n"
        "a newline is always written (column tracking is not yet\n"
        "implemented for all stream types).\n"
        "Returns non-nil if a newline was written, nil otherwise.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-finish-output", Ffinish_output, 0, 1,
        "Attempt to ensure that all output sent to STREAM has reached\n"
        "its destination, and then return.\n"
        "\n"
        "For file streams this calls fsync(2).\n"
        "STREAM defaults to `*standard-output*'.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-force-output", Fforce_output, 0, 1,
        "Initiate the emptying of any internal output buffers on STREAM.\n"
        "\n"
        "For file streams this calls fflush(3).\n"
        "STREAM defaults to `*standard-output*'.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-clear-output", Fclear_output, 0, 1,
        "Abort any outstanding output operation on STREAM.\n"
        "\n"
        "Any buffered output that has not yet been written is discarded.\n"
        "STREAM defaults to `*standard-output*'.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-clear-input", Fclear_input, 0, 1,
        "Clear any buffered input on STREAM.\n"
        "\n"
        "Discards any characters in the stream's read buffer and\n"
        "pushback ring.\n"
        "STREAM defaults to `*standard-input*'.\n"
        "\n(fn &optional STREAM)", NULL);

  defun(env, "cl-write-sequence", Fwrite_sequence, 2, 6,
        "Write the elements of SEQUENCE to STREAM.\n"
        "\n"
        "SEQUENCE must be a vector or list.  Writing starts at :START\n"
        "and stops at :END (default the length of SEQUENCE).\n"
        "Returns SEQUENCE.\n"
         "\n(fn SEQUENCE STREAM &key START END)", NULL);

  /* ── file I/O and streams ────────────────────────────────────────── */
  defun(env, "cl-open", Fopen, 1, 8,
        "Open a file and return a stream connected to it.\n"
        "\n"
        "FILENAME is a string naming the file.\n"
        "Keyword arguments:\n"
        "  :DIRECTION   — :INPUT (default), :OUTPUT, :IO, or :PROBE\n"
        "  :ELEMENT-TYPE — character or (unsigned-byte 8)\n"
        "  :IF-EXISTS   — :ERROR, :SUPERSEDE, :APPEND, :OVERWRITE, NIL\n"
        "  :IF-DOES-NOT-EXIST — :ERROR, :CREATE, or NIL\n"
        "  :EXTERNAL-FORMAT — accepted for compatibility, currently ignored\n"
        "PROBE returns a closed stream if the file exists, NIL otherwise.\n"
        "\n(fn FILENAME &key DIRECTION ELEMENT-TYPE IF-EXISTS IF-DOES-NOT-EXIST EXTERNAL-FORMAT)", NULL);

  defun(env, "cl-close", Fclose, 1, 3,
        "Close STREAM.\n"
        "\n"
        "If :ABORT is non-nil, pending output is discarded without flushing.\n"
        "Closing an already-closed stream is harmless and returns t.\n"
        "\n(fn STREAM &key ABORT)", NULL);

  defun(env, "cl-make-string-input-stream", Fmake_string_input, 1, 3,
        "Return a stream which reads characters from STRING.\n"
        "\n"
        "Optional START and END arguments delimit the substring to read.\n"
        "The returned stream is an input stream with element type CHARACTER.\n"
        "\n(fn STRING &optional START END)", NULL);

  defun(env, "cl-make-string-output-stream", Fmake_string_output, 0, 5,
        "Return a stream which accumulates output in a string.\n"
        "\n"
        "The accumulated string can be retrieved with\n"
        "`cl-get-output-stream-string'.\n"
        "The :ELEMENT-TYPE keyword is accepted for compatibility but\n"
        "currently ignored; the element type is always CHARACTER.\n"
        ":SIZE N pre-allocates a single block of N bytes, eliminating\n"
        "allocation overhead during writes.  Use when the output size\n"
        "is known in advance.\n"
        "\n(fn &key ELEMENT-TYPE SIZE)", NULL);

  defun(env, "cl-get-output-stream-string", Fget_output_stream_string, 1, 1,
        "Return the accumulated string from a string output stream.\n"
        "\n"
        "STREAM must have been created by `cl-make-string-output-stream'.\n"
        "The stream remains open and may be written to further.\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-make-buffer-input-stream", Fmake_buffer_input_stream, 1, 5,
        "Return a stream which reads characters from BUFFER.\n"
        "\n"
        "BUFFER may be a buffer object or a buffer name.\n"
        "Keywords :START and :END delimit the region to read.\n"
        "The stream reads buffer text in 64 KB chunks for efficiency.\n"
        "\n(fn BUFFER &key START END)", NULL);

  /* ── predicates ──────────────────────────────────────────────────── */
  defun(env, "cl-streamp", Fstreamp, 1, 1,
        "Return t if OBJECT is a stream (open or closed).\n"
        "\n(fn OBJECT)", NULL);

  defun(env, "cl-open-stream-p", Fopen_stream_p, 1, 1,
        "Return t if OBJECT is an open stream, nil otherwise.\n"
        "\n(fn OBJECT)", NULL);

  defun(env, "cl-input-stream-p", Finput_stream_p, 1, 1,
        "Return t if STREAM is an input stream, nil otherwise.\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-output-stream-p", Foutput_stream_p, 1, 1,
        "Return t if STREAM is an output stream, nil otherwise.\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-stream-element-type", Fstream_element_type, 1, 1,
        "Return a type specifier for the kind of object returned by\n"
        "`cl-read-char' on STREAM.\n"
        "\n"
         "Currently returns `character' or the list `(unsigned-byte 8)'.\n"
         "\n(fn STREAM)", NULL);

  defun(env, "cl-file-regular-p", Ffile_regular_p, 1, 1,
        "Return t if STREAM is backed by a regular file (S_ISREG).\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-file-tty-p", Ffile_tty_p, 1, 1,
        "Return t if STREAM is connected to a terminal (isatty).\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-interactive-stream-p", Finteractive_stream_p, 1, 1,
        "Return t if STREAM is an interactive stream.\n"
        "\n"
        "An interactive stream is one connected to a terminal\n"
        "(i.e., isatty).\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-stream-external-format", Fstream_external_format, 1, 1,
        "Return the external format of STREAM.\n"
        "\n"
        "Currently always returns :default as encoding is not\n"
        "implemented.\n"
        "\n(fn STREAM)", NULL);

  defun(env, "cl-file-position", Ffile_position, 1, 3,
        "Return the current position of STREAM, or set it.\n"
        "\n"
        "With one argument, return the current position as a\n"
        "non-negative integer, or nil if the stream does not support\n"
        "position queries.\n"
        "With a second argument POSITION, set the position to that\n"
        "value and return t on success, nil on failure.\n"
        "\n(fn STREAM &optional POSITION)", NULL);

  defun(env, "cl-file-length", Ffile_length, 1, 1,
        "Return the length of the file associated with STREAM.\n"
        "\n"
        "Returns a non-negative integer if the stream supports length\n"
        "queries, or nil if the length cannot be determined.\n"
        "\n(fn STREAM)", NULL);

  /* ── Lisp reader / printer ───────────────────────────────────────── */
  defun(env, "cl-read", Fread, 0, 4,
        "Read one Lisp object from STREAM and return it.\n"
        "\n"
        "Reads characters from the stream and feeds them to Emacs'\n"
        "`read-from-string' incrementally until a complete form is parsed.\n"
        "STREAM defaults to `*standard-input*'.\n"
        "If EOF-ERROR-P is nil and end of file is reached before a complete\n"
        "form, return EOF-VALUE.  Otherwise signal an error on EOF.\n"
        "RECURSIVE-P is accepted for compatibility and currently ignored.\n"
        "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);

  defun(env, "cl-read-from-string", Fread_from_string, 1, 7,
        "Read one Lisp object from STRING and return (VALUE . POSITION).\n"
        "\n"
        "Delegates to Emacs' `read-from-string'.\n"
        "Keywords :START and :END delimit the substring to read.\n"
        "If :PRESERVE-WHITESPACE is non-nil, trailing whitespace is kept\n"
        "in the secondary return value (delegated to Emacs reader).\n"
         "\n(fn STRING &optional EOF-ERROR-P EOF-VALUE &key START END PRESERVE-WHITESPACE)", NULL);

  defun(env, "cl-read-preserving-whitespace", Fread_preserving_whitespace, 0, 4,
        "Read one Lisp object from STREAM, preserving trailing whitespace.\n"
        "\n"
        "Like `cl-read' but does not consume trailing whitespace.\n"
        "\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);

  defun(env, "cl-read-delimited-list", Fread_delimited_list, 1, 3,
        "Read objects from STREAM until CHAR is seen, return as a list.\n"
        "\n"
        "CHAR is the delimiter character (an integer).\n"
        "STREAM defaults to `*standard-input*'.\n"
        "The delimiter is left on the stream (unread).\n"
        "\n(fn CHAR &optional STREAM RECURSIVE-P)", NULL);

  defun(env, "cl-write", Fwrite, 1, 15,
        "Write OBJECT to STREAM.\n"
        "\n"
        "Accepts standard CL print-control keyword arguments that bind\n"
        "the corresponding Emacs print-* variables during output:\n"
        "  :ESCAPE → print-escape\n"
        "  :RADIX → print-radix    :BASE → print-base\n"
        "  :CIRCLE → print-circle  :PRETTY → print-pretty\n"
        "  :LEVEL → print-level    :LENGTH → print-length\n"
        "  :CASE → print-case      :GENSYM → print-gensym\n"
        "  :ARRAY → print-array    :READABLY → print-readably\n"
        "\n(fn OBJECT &key STREAM ESCAPE RADIX BASE CIRCLE PRETTY LEVEL LENGTH CASE GENSYM ARRAY READABLY RIGHT-MARGIN MISER-WIDTH PPRINT-DISPATCH LINES)", NULL);

  defun(env, "cl-prin1", Fprin1, 1, 2,
        "Output OBJECT to STREAM in a form suitable for `cl-read'.\n"
        "\n"
        "Delegates to Emacs' `prin1-to-string'.\n"
        "STREAM defaults to `*standard-output*'.\n"
        "Returns OBJECT.\n"
        "\n(fn OBJECT &optional STREAM)", NULL);

  defun(env, "cl-princ", Fprinc, 1, 2,
        "Output OBJECT to STREAM in a human-readable form.\n"
        "\n"
        "For strings, output is written without surrounding quotes;\n"
        "for other objects the behavior matches `cl-prin1'.\n"
        "STREAM defaults to `*standard-output*'.\n"
        "Returns OBJECT.\n"
        "\n(fn OBJECT &optional STREAM)", NULL);

  defun(env, "cl-print", Fprint, 1, 2,
        "Output a newline, then OBJECT (via PRIN1), then a space, to STREAM.\n"
        "\n"
        "STREAM defaults to `*standard-output*'.\n"
        "Returns OBJECT.\n"
        "\n(fn OBJECT &optional STREAM)", NULL);

  defun(env, "cl-pprint", Fpprint, 1, 2,
        "Pretty-print OBJECT to STREAM.\n"
        "\n"
        "Delegates to Emacs' `pp-to-string' for formatting.\n"
        "STREAM defaults to `*standard-output*'.\n"
        "\n(fn OBJECT &optional STREAM)", NULL);

  defun(env, "cl-prin1-to-string", Fprin1_to_string, 1, 1,
        "Return a string containing OBJECT printed in a form suitable\n"
        "for `cl-read'.\n"
        "\n"
        "Delegates to Emacs' `prin1-to-string'.\n"
        "\n(fn OBJECT)", NULL);

  defun(env, "cl-princ-to-string", Fprinc_to_string, 1, 1,
        "Return a string containing OBJECT printed in a human-readable\n"
        "form.\n"
        "\n"
        "For strings the result lacks surrounding quotes.\n"
        "\n(fn OBJECT)", NULL);

  defun(env, "cl-write-to-string", Fwrite_to_string, 1, 15,
        "Write OBJECT to a string and return it.\n"
        "\n"
        "Like `cl-write' but returns the output as a string instead\n"
        "of writing to a stream.  Accepts the same keyword arguments\n"
        "as `cl-write'.\n"
        "\n(fn OBJECT &key ESCAPE RADIX BASE CIRCLE PRETTY LEVEL LENGTH CASE GENSYM ARRAY READABLY)", NULL);

  /* ── standard streams ────────────────────────────────────────────── */
  init_standard_streams(env);

  provide(env, "emacs-io");
  return 0;
}

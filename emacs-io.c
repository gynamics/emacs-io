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
extern emacs_value Fpeek_char(emacs_env *env, ptrdiff_t nargs,
                               emacs_value *args, void *data);
extern emacs_value Funread_char(emacs_env *env, ptrdiff_t nargs,
                                 emacs_value *args, void *data);
extern emacs_value Flisten(emacs_env *env, ptrdiff_t nargs,
                            emacs_value *args, void *data);
extern emacs_value Fread_sequence(emacs_env *env, ptrdiff_t nargs,
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

/* lispio.c */
extern emacs_value Fread(emacs_env *env, ptrdiff_t nargs,
                          emacs_value *args, void *data);
extern emacs_value Fread_from_string(emacs_env *env, ptrdiff_t nargs,
                                      emacs_value *args, void *data);
extern emacs_value Fread_preserving_whitespace(emacs_env *env, ptrdiff_t nargs,
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

/* stdstream.c */
extern void init_standard_streams(emacs_env *env);

/* ── module entry point ───────────────────────────────────────────────── */

int emacs_module_init(struct emacs_runtime *runtime) {
  emacs_env *env = runtime->get_environment(runtime);

  /* ensure we have at least emacs_env_31 */
  if ((size_t)env->size < sizeof(struct emacs_env_31)) return 1;

  /* ── reader ──────────────────────────────────────────────────────── */
  defun(env, "cl-read-char", Fread_char, 0, 3,
        "Read one character from STREAM.\n\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE)", NULL);
  defun(env, "cl-read-line", Fread_line, 0, 3,
        "Read a line of text from STREAM.\n\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE)", NULL);
  defun(env, "cl-read-byte", Fread_byte, 1, 3,
        "Read one byte from STREAM.\n\n(fn STREAM &optional EOF-ERROR-P EOF-VALUE)", NULL);
  defun(env, "cl-peek-char", Fpeek_char, 0, 4,
        "Peek at next character from STREAM.\n\n(fn &optional PEEK-TYPE STREAM EOF-ERROR-P EOF-VALUE)", NULL);
  defun(env, "cl-unread-char", Funread_char, 1, 2,
        "Return CHARACTER to STREAM.\n\n(fn CHARACTER &optional STREAM)", NULL);
  defun(env, "cl-listen", Flisten, 0, 1,
        "Return t if a character is available on STREAM.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-read-sequence", Fread_sequence, 2, 6,
        "Read elements into SEQUENCE from STREAM.\n\n(fn SEQUENCE STREAM &key START END)", NULL);

  /* ── printer ─────────────────────────────────────────────────────── */
  defun(env, "cl-write-char", Fwrite_char, 1, 2,
        "Write CHARACTER to STREAM.\n\n(fn CHARACTER &optional STREAM)", NULL);
  defun(env, "cl-write-string", Fwrite_string, 1, 5,
        "Write STRING to STREAM.\n\n(fn STRING &optional STREAM &key START END)", NULL);
  defun(env, "cl-write-line", Fwrite_line, 1, 5,
        "Write STRING followed by a newline to STREAM.\n\n(fn STRING &optional STREAM &key START END)", NULL);
  defun(env, "cl-write-byte", Fwrite_byte, 2, 2,
        "Write BYTE to STREAM.\n\n(fn BYTE STREAM)", NULL);
  defun(env, "cl-terpri", Fterpri, 0, 1,
        "Output a newline to STREAM.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-fresh-line", Ffresh_line, 0, 1,
        "Output a newline if not at start of line.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-finish-output", Ffinish_output, 0, 1,
        "Ensure all output sent to STREAM reaches its destination.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-force-output", Fforce_output, 0, 1,
        "Flush buffered output to STREAM.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-clear-output", Fclear_output, 0, 1,
        "Clear pending output on STREAM.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-clear-input", Fclear_input, 0, 1,
        "Clear pending input on STREAM.\n\n(fn &optional STREAM)", NULL);
  defun(env, "cl-write-sequence", Fwrite_sequence, 2, 6,
        "Write elements of SEQUENCE to STREAM.\n\n(fn SEQUENCE STREAM &key START END)", NULL);

  /* ── file I/O and streams ────────────────────────────────────────── */
  defun(env, "cl-open", Fopen, 1, 6,
        "Open a file, returning a stream.\n\n(fn FILENAME &key DIRECTION ELEMENT-TYPE IF-EXISTS IF-DOES-NOT-EXIST EXTERNAL-FORMAT)", NULL);
  defun(env, "cl-close", Fclose, 1, 1,
        "Close STREAM.\n\n(fn STREAM)", NULL);
  defun(env, "cl-make-string-input-stream", Fmake_string_input, 1, 3,
        "Return a stream which reads from STRING.\n\n(fn STRING &optional START END)", NULL);
  defun(env, "cl-make-string-output-stream", Fmake_string_output, 0, 1,
        "Return a stream which accumulates output in a string.\n\n(fn &key ELEMENT-TYPE)", NULL);
  defun(env, "cl-get-output-stream-string", Fget_output_stream_string, 1, 1,
        "Return the accumulated string from a string output stream.\n\n(fn STREAM)", NULL);
  defun(env, "cl-make-buffer-input-stream", Fmake_buffer_input_stream, 1, 5,
        "Return a stream which reads from BUFFER.\n\n(fn BUFFER &key START END)", NULL);

  /* ── predicates ──────────────────────────────────────────────────── */
  defun(env, "cl-open-stream-p", Fopen_stream_p, 1, 1,
        "Return t if OBJECT is an open stream.\n\n(fn OBJECT)", NULL);
  defun(env, "cl-input-stream-p", Finput_stream_p, 1, 1,
        "Return t if STREAM is an input stream.\n\n(fn STREAM)", NULL);
  defun(env, "cl-output-stream-p", Foutput_stream_p, 1, 1,
        "Return t if STREAM is an output stream.\n\n(fn STREAM)", NULL);
  defun(env, "cl-stream-element-type", Fstream_element_type, 1, 1,
        "Return the element type of STREAM.\n\n(fn STREAM)", NULL);

  /* ── Lisp reader / printer ───────────────────────────────────────── */
  defun(env, "cl-read", Fread, 0, 4,
        "Read one Lisp object from STREAM.\n\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);
  defun(env, "cl-read-from-string", Fread_from_string, 1, 7,
        "Read a Lisp object from STRING.\n\n(fn STRING &optional EOF-ERROR-P EOF-VALUE &key START END PRESERVE-WHITESPACE)", NULL);
  defun(env, "cl-read-preserving-whitespace", Fread_preserving_whitespace, 0, 4,
        "Read preserving whitespace.\n\n(fn &optional STREAM EOF-ERROR-P EOF-VALUE RECURSIVE-P)", NULL);
  defun(env, "cl-write", Fwrite, 1, 15,
        "Write OBJECT to STREAM.\n\n(fn OBJECT &key STREAM ...)", NULL);
  defun(env, "cl-prin1", Fprin1, 1, 2,
        "Output OBJECT to STREAM in machine-readable form.\n\n(fn OBJECT &optional STREAM)", NULL);
  defun(env, "cl-princ", Fprinc, 1, 2,
        "Output OBJECT to STREAM in human-readable form.\n\n(fn OBJECT &optional STREAM)", NULL);
  defun(env, "cl-print", Fprint, 1, 2,
        "Output OBJECT to STREAM with a leading newline.\n\n(fn OBJECT &optional STREAM)", NULL);
  defun(env, "cl-prin1-to-string", Fprin1_to_string, 1, 1,
        "Return a string containing OBJECT in machine-readable form.\n\n(fn OBJECT)", NULL);
  defun(env, "cl-princ-to-string", Fprinc_to_string, 1, 1,
        "Return a string containing OBJECT in human-readable form.\n\n(fn OBJECT)", NULL);

  /* ── standard streams ────────────────────────────────────────────── */
  init_standard_streams(env);

  provide(env, "emacs-io");
  return 0;
}

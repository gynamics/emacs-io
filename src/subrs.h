#ifndef EMACS_IO_SUBRS_H
#define EMACS_IO_SUBRS_H

#include "emacs-module.h"
#include <stddef.h>

/* ── registered subr entry points ───────────────────────────────────────
   Every `cl-<name>' function is registered from a table in emacs-io.c.
   Declaring the C functions here (grouped by the file that defines them)
   keeps emacs-io.c free of hand-written forward declarations and lets each
   defining file get a compile-time signature check. */

#define CL_SUBR(name) \
  extern emacs_value name(emacs_env *env, ptrdiff_t nargs,                \
                          emacs_value *args, void *data)

/* reader.c */
CL_SUBR(Fread_char);
CL_SUBR(Fread_line);
CL_SUBR(Fread_byte);
CL_SUBR(Fread_char_no_hang);
CL_SUBR(Fpeek_char);
CL_SUBR(Funread_char);
CL_SUBR(Flisten);
CL_SUBR(Fread_sequence);
CL_SUBR(Fread_string);
CL_SUBR(Fread_chunk);
CL_SUBR(Fread_delimited_list);
CL_SUBR(Fy_or_n_p);
CL_SUBR(Fyes_or_no_p);

/* printer.c */
CL_SUBR(Fwrite_char);
CL_SUBR(Fwrite_string);
CL_SUBR(Fwrite_line);
CL_SUBR(Fwrite_byte);
CL_SUBR(Fterpri);
CL_SUBR(Ffresh_line);
CL_SUBR(Ffinish_output);
CL_SUBR(Fforce_output);
CL_SUBR(Fclear_output);
CL_SUBR(Fclear_input);
CL_SUBR(Fwrite_sequence);

/* fileio.c */
CL_SUBR(Fopen);
CL_SUBR(Fclose);
CL_SUBR(Fstreamp);
CL_SUBR(Fopen_stream_p);
CL_SUBR(Finput_stream_p);
CL_SUBR(Foutput_stream_p);
CL_SUBR(Fstream_element_type);
CL_SUBR(Ffile_regular_p);
CL_SUBR(Ffile_tty_p);
CL_SUBR(Ffile_position);
CL_SUBR(Ffile_length);
CL_SUBR(Finteractive_stream_p);
CL_SUBR(Fstream_external_format);
CL_SUBR(Ffile_string_length);
CL_SUBR(Fftruncate);

/* lispio.c */
CL_SUBR(Fread);
CL_SUBR(Fread_preserving_whitespace);
CL_SUBR(Fread_from_string);
CL_SUBR(Fwrite);
CL_SUBR(Fprin1);
CL_SUBR(Fprinc);
CL_SUBR(Fprint);
CL_SUBR(Fpprint);
CL_SUBR(Fprin1_to_string);
CL_SUBR(Fprinc_to_string);
CL_SUBR(Fwrite_to_string);

/* compstream.c */
CL_SUBR(Fmake_synonym_stream);
CL_SUBR(Fsynonym_stream_symbol);
CL_SUBR(Fmake_broadcast_stream);
CL_SUBR(Fbroadcast_stream_streams);
CL_SUBR(Fmake_concatenated_stream);
CL_SUBR(Fconcatenated_stream_streams);
CL_SUBR(Fmake_two_way_stream);
CL_SUBR(Ftwo_way_stream_input_stream);
CL_SUBR(Ftwo_way_stream_output_stream);
CL_SUBR(Fmake_echo_stream);
CL_SUBR(Fecho_stream_input_stream);
CL_SUBR(Fecho_stream_output_stream);

/* strstream.c */
CL_SUBR(Fmake_string_input);
CL_SUBR(Fmake_string_output);
CL_SUBR(Fget_output_stream_string);

/* bufstream.c */
CL_SUBR(Fmake_buffer_input_stream);
CL_SUBR(Fmake_buffer_output_stream);
CL_SUBR(Fmake_buffer_io_stream);

#undef CL_SUBR

/* stdstream.c */
extern void init_standard_streams(emacs_env *env);

#endif /* EMACS_IO_SUBRS_H */

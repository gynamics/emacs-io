;;; emacs-io.el --- CL-style stream I/O for GNU Emacs  -*- lexical-binding: t; -*-

;; Copyright (C) 2026  Free Software Foundation, Inc.

;; Author: emacs-io contributors
;; Version: 1.0
;; Package-Requires: ((emacs "31"))
;; Keywords: files, extensions

;; This file is not part of GNU Emacs.

;;; Commentary:

;; Entry point for the emacs-io package.  It loads the native core module
;; (`emacs-io-core', built as emacs-io-core.so), which provides the
;; byte/character/sequence primitives, the stream constructors, the
;; predicates, and the object reader (`cl-read' /
;; `cl-read-preserving-whitespace', which drive Emacs' reader from C through a
;; module function stream).  The functions that are just glue over Emacs' own
;; reader and printer are defined here in Lisp, together with the
;; `cl-with-open-file' / `cl-with-open-stream' macros.
;;
;; Put this directory on `load-path' and `(require 'emacs-io)'.

;;; Code:

(require 'cl-lib)
(require 'emacs-io-core)

;; C primitives provided by emacs-io-core.so (declared so this file
;; byte-compiles cleanly when the module has not been loaded).
(declare-function cl-write-string "emacs-io-core"
                  (string &optional stream &key start end))
(declare-function cl-write-char "emacs-io-core" (character &optional stream))
(declare-function cl-read-line "emacs-io-core"
                  (&optional stream eof-error-p eof-value recursive-p))
(declare-function cl-read-char "emacs-io-core"
                  (&optional stream eof-error-p eof-value recursive-p))
(declare-function cl-unread-char "emacs-io-core" (character &optional stream))
(declare-function cl--whitespace-p "emacs-io-core" (character))
(declare-function cl-read "emacs-io-core"
                  (&optional stream eof-error-p eof-value recursive-p))
(declare-function cl-read-preserving-whitespace "emacs-io-core"
                  (&optional stream eof-error-p eof-value recursive-p))
(declare-function cl-peek-char "emacs-io-core"
                  (&optional peek-type stream eof-error-p eof-value recursive-p))
(declare-function cl-make-string-output-stream "emacs-io-core"
                  (&rest keys))
(declare-function cl-get-output-stream-string "emacs-io-core" (stream))

;;;; Reader

;; cl-read / cl-read-preserving-whitespace live in the C core: they drive
;; Emacs' own reader through a module function stream (no Lisp callback per
;; character), and the CL whitespace set is defined once in C.

(cl-defun cl-read-from-string (string &optional (eof-error-p t) eof-value
                                      &key start end preserve-whitespace
                                      &allow-other-keys)
  "Read one Lisp object from STRING and return (VALUE . POSITION).
:START and :END are CL bounding indices delimiting the substring to
read; an out-of-range index signals `args-out-of-range'.
If :PRESERVE-WHITESPACE is non-nil, the delimiting whitespace is not
counted in POSITION."
  (let* ((end (or end (length string)))
         (result (condition-case nil
                     (read-from-string string (or start 0) end)
                   (end-of-file :eof))))
    (if (eq result :eof)
        (if eof-error-p
            (signal 'end-of-file '("End of file on string"))
          eof-value)
      (let ((pos (cdr result)))
        ;; CL: `read-from-string' consumes the single whitespace character
        ;; that delimits the object, so it is counted in POSITION (e.g.
        ;; (read-from-string " 1 3 5" t nil :start 2) => 3, 5).  The
        ;; whitespace test is the one C definition.
        (when (and (not preserve-whitespace)
                   (< pos end)
                   (cl--whitespace-p (aref string pos)))
          (setq pos (1+ pos)))
        (cons (car result) pos)))))

(cl-defun cl-read-delimited-list (char &optional stream recursive-p)
  "Read objects from STREAM until CHAR is seen, and return them as a list.
The delimiter is consumed; reaching end of file is an error."
  (ignore recursive-p)
  (let ((lst '()) (done nil))
    (while (not done)
      (let ((c (cl-peek-char t stream nil :eof)))
        (cond ((eq c :eof)
               (signal 'end-of-file '("End of file in read-delimited-list")))
              ((eql c char)
               (cl-read-char stream)
               (setq done t))
              (t (push (cl-read stream) lst)))))
    (nreverse lst)))

(defun cl--query-io ()
  (let ((s (and (boundp '*query-io*) (symbol-value '*query-io*))))
    (or s (error "*query-io* is not an open stream"))))

(defun cl--ask-query (control args suffix yes-p)
  (let ((s (cl--query-io)))
    (when control
      (cl-write-string (apply #'format control args) s)
      (cl-write-string suffix s))
    (let* ((line (car (cl-read-line s nil "")))
           (resp (if (stringp line) line "")))
      (funcall yes-p resp))))

(defun cl-y-or-n-p (&optional control &rest arguments)
  "Ask a yes/no question on *query-io*.  Return t for \"yes\", nil for \"no\"."
  (cl--ask-query control arguments " (y/n) "
                 (lambda (r) (and (> (length r) 0)
                                  (memq (aref r 0) '(?y ?Y))
                                  t))))

(defun cl-yes-or-no-p (&optional control &rest arguments)
  "Ask a yes/no question requiring a full \"yes\" answer on *query-io*."
  (cl--ask-query control arguments " (yes/no) "
                 (lambda (r) (and (>= (length r) 3)
                                  (memq (aref r 0) '(?y ?Y))
                                  (memq (aref r 1) '(?e ?E))
                                  (memq (aref r 2) '(?s ?S))
                                  t))))

;;;; Printer

(defun cl-io-prin1-to-string (object)
  "Return a string containing OBJECT printed in a form suitable for `cl-read'.
Like Emacs' `cl-prin1-to-string', but distinct so it does not shadow it."
  (prin1-to-string object))

(defun cl--princ-string (object)
  ;; princ writes strings without their surrounding quotes.
  (if (stringp object) object (prin1-to-string object)))

(defun cl-princ-to-string (object)
  "Return a string containing OBJECT printed in a human-readable form."
  (cl--princ-string object))

(defun cl-io-prin1 (object &optional stream)
  "Output OBJECT to STREAM in a form suitable for `cl-read'.  Return OBJECT.
Like Emacs' `cl-prin1', but writes to an emacs-io stream."
  (cl-write-string (prin1-to-string object) stream)
  object)

(defun cl-princ (object &optional stream)
  "Output OBJECT to STREAM in a human-readable form.  Return OBJECT."
  (cl-write-string (cl--princ-string object) stream)
  object)

(defun cl-print (object &optional stream)
  "Output a newline, then OBJECT (via PRIN1), then a space, to STREAM.
Return OBJECT."
  (cl-write-char ?\n stream)
  (cl-io-prin1 object stream)
  (cl-write-char ?\s stream)
  object)

(defun cl-pprint (object &optional stream)
  "Pretty-print OBJECT to STREAM (returns nil, as CL specifies no values)."
  (cl-write-string (pp-to-string object) stream)
  nil)

(defun cl-terpri (&optional stream)
  "Output a newline character to STREAM.  Return nil."
  (cl-write-char ?\n stream)
  nil)

(cl-defun cl-write (object &key (stream nil stream-p)
                           (escape nil escape-p)
                           (pretty nil pretty-p)
                           (circle nil circle-p)
                           (level nil level-p)
                           (length nil length-p)
                           (gensym nil gensym-p)
                           base radix case array readably
                           right-margin miser-width pprint-dispatch lines
                           &allow-other-keys)
  "Write OBJECT to STREAM (default `*standard-output*').
Keywords with an Emacs counterpart are bound during output: :CIRCLE
(print-circle), :LEVEL, :LENGTH and :GENSYM.  :ESCAPE nil prints without
escaping; :PRETTY t uses Emacs' pretty-printer.  The remaining CL controls
are accepted and ignored."
  (ignore base radix case array readably right-margin miser-width
          pprint-dispatch lines)
  (let ((s (if stream-p stream (symbol-value '*standard-output*)))
        (print-circle (if circle-p circle print-circle))
        (print-level  (if level-p  level  print-level))
        (print-length (if length-p length print-length))
        (print-gensym (if gensym-p gensym print-gensym)))
    (cl-write-string
     (cond ((and escape-p (null escape)) (format "%s" object))
           ((and pretty-p pretty) (pp-to-string object))
           (t (prin1-to-string object)))
     s))
  object)

(defun cl-write-to-string (object &rest keys)
  "Write OBJECT to a string and return it.  Accepts `cl-write' keywords."
  (let ((s (cl-make-string-output-stream)))
    (apply #'cl-write object :stream s keys)
    (cl-get-output-stream-string s)))

;;;; Introspection

(defun cl-file-string-length (stream object)
  "Return the byte count of OBJECT's printed representation on STREAM."
  (ignore stream)
  (string-bytes (prin1-to-string object)))

;;;; Convenience macros

;;;###autoload
(cl-defmacro cl-with-open-file ((stream filespec &rest options) &body body)
  "Like CL's with-open-file: open FILESPEC, bind STREAM, evaluate BODY, close.
OPTIONS are passed to `cl-open'."
  (declare (indent 1) (debug ((symbolp form &rest form) body)))
  `(let ((,stream (cl-open ,filespec ,@options)))
     (unwind-protect
         (progn ,@body)
       (when ,stream
         (cl-close ,stream)))))

;;;###autoload
(cl-defmacro cl-with-open-stream ((var stream) &body body)
  "Like CL's with-open-stream: bind VAR to STREAM, evaluate BODY, close."
  (declare (indent 1) (debug ((symbolp form) body)))
  `(let ((,var ,stream))
     (unwind-protect
         (progn ,@body)
       (cl-close ,var))))

;; `cl-print' and `cl-extra' already define `cl-prin1', `cl-prin1-to-string'
;; and `cl-fresh-line'; to avoid shadowing Emacs' `cl-lib', our stream-aware
;; counterparts are named `cl-io-prin1', `cl-io-prin1-to-string' and
;; `cl-io-fresh-line' instead.

(provide 'emacs-io)
;;; emacs-io.el ends here

;;; emacs-io-macros.el --- CL-style convenience macros -*- lexical-binding: t; -*-

(require 'cl-lib)

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

(provide 'emacs-io-macros)
;;; emacs-io-macros.el ends here

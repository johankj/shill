;; -----------------------------------------------------------------------------
;; Shill mode
;; -----------------------------------------------------------------------------

;; Keywords.
(defvar shill-keywords
  '(
    "+optional"
    "+rest"
    "->"
    "<-"
    "any"
    "as"
    "do"
    "else"
    "false"
    "for"
    "forall"
    "fun"
    "if"
    "in"
    "init"
    "only"
    "provide"
    "require"
    "then"
    "true"
    "update"
    "val"
    "vals"
    "values"
    "var"
    "void"
    "when"
    "while"
    "with"
    )
  "List of built-in Shill keywords."
  )

;; Operators.
(defvar shill-operators
  '(
    "="
    "++"
    "&"
    "|"
    "and"
    "not"
    "or"
    "*"
    "*="
    "+"
    "+="
    "-"
    "-="
    "/"
    "/="
    "<"
    "<="
    "=="
    ">"
    ">="
    )
  "List of Shill operators."
  )

;; Operators and punctuation characters that are not handled correctly by `regexp-opt'.
;; Includes: /\, \/, :=, : and .
(defvar shill-special-operators-regexp "\\\\/\\|/\\\\\\|:=\\|:\\|\\.\\|")

;; Capabilities.
(defvar shill-capabilities
  '(
    "dir"
    "file"
    "pipe-end"
    "pipe-factory"
    "socket-factory"
    )
  "List of Shill capabilities."
  )

;; Tab width.
(defvar shill-tab-width 4 "Default tab width for Shill files.")

;; Syntax higlighting defaults.
(defvar shill-font-lock-defaults
  `((
     ;; Punctuation characters and operators.
     (,(concat shill-special-operators-regexp (regexp-opt shill-operators 'symbols)) . font-lock-builtin-face)
     ;; Strings.
     ("\".*\"" . font-lock-string-face)
     ;; Keywords.
     (,(regexp-opt shill-keywords 'symbols) . font-lock-keyword-face)
     ;; Privileges.
     ("\\+[a-zA-Z-]+" . font-lock-constant-face)
     ;; Capabilities.
     ("\\([a-zA-Z0-9-]+/c\\)(?\\b" . '(1 font-lock-builtin-face))
     (,(regexp-opt shill-capabilities 'symbols) . font-lock-builtin-face)
     ;; Functions.
     ("\\([a-zA-Z0-9->]+\\)(" . '(1 font-lock-function-name-face))
     ;; Predicates.
     ("\\([a-zA-Z0-9-]+\\?\\)(?" . '(1 font-lock-function-name-face))
     ))
  "Determines how different keywords, functions, operators etc. are syntax highlighted in Shill files.")

;; Indentation.
(defun shill-indent-line ()
  "Indent the current line."
  (interactive)
  (if (= 1 (line-number-at-pos))
      (indent-line-to 0)
    (let* ((above-line-no-and-indent-pos
            (save-mark-and-excursion
              (previous-line)
              (back-to-indentation)
              (if (and (or (looking-at "[[:space:]]*$") (looking-at "#"))
                       (/= (line-number-at-pos) 1))
                  (progn
                    (re-search-backward "[[:graph:]]")
                    (back-to-indentation)
                    (while (and (looking-at "#")
                                (/= (line-number-at-pos) 1))
                      (re-search-backward "[[:graph:]]")
                      (back-to-indentation))))
              (cons (line-number-at-pos) (current-indentation))))
           (occ-end-par
            (save-mark-and-excursion
             (back-to-indentation)
             (looking-at "}")))
           (above-no-occ-start-par
            (save-mark-and-excursion
             (goto-line (car above-line-no-and-indent-pos))
             (how-many "{" (point) (line-end-position))))
           (above-no-occ-end-par
            (save-mark-and-excursion
             (goto-line (car above-line-no-and-indent-pos))
             (back-to-indentation)
             (if (and (looking-at "}")
                      (or (= (line-end-position) (+ 1 (point))) ; For single '}'.
                          (= (line-end-position) (+ 2 (point))) ; For single '};.
                          (re-search-forward "else")))  ; For 'else' keyword on same line.
                 0
               (how-many "}" (point) (line-end-position))))))
      (save-mark-and-excursion
       (indent-line-to (max (- (+ (cdr above-line-no-and-indent-pos)
                                  (* (- above-no-occ-start-par above-no-occ-end-par) shill-tab-width))
                               (if occ-end-par shill-tab-width 0))
                            0)))
      (if (< (current-column) (current-indentation))
          (back-to-indentation)))))

;; Definition of Shill mode.
(define-derived-mode shill-mode prog-mode "Shill"
  "Shill mode is a major mode for editing Shill files."
  ;; Set syntax highlighting.
  (setq-local font-lock-defaults shill-font-lock-defaults)
  ;; Set tab width.
  (setq-local tab-width shill-tab-width)
  ;; Set indentation.
  (setq-local indent-line-function 'shill-indent-line)
  ;; Set comment syntax.
  (setq-local comment-start "#")
  (setq-local comment-end "")
  (modify-syntax-entry ?# "< b" shill-mode-syntax-table)
  (modify-syntax-entry ?\n "> b" shill-mode-syntax-table))

;; Automatically open .amb and .cap files in Shill mode.
(add-to-list 'auto-mode-alist '("\\.\\(amb\\|cap\\)\\'" . shill-mode))

;; -----------------------------------------------------------------------------
(provide 'shill-mode)

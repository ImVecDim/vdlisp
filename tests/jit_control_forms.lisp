;; JIT control forms test: exercises cond, let, while inside functions and triggers JIT

;; cond
(set f_cond (fn (x) (cond ((> x 0) x) (#t (- x)))))
(f_cond 1)
(f_cond 2)
(f_cond 3)
(f_cond 4)
(f_cond 5)
(type f_cond)
(print f_cond)
(print "COND_DONE")

;; let
(set f_let (fn (x) (let (a x) (+ a 1))))
(f_let 1)
(f_let 2)
(f_let 3)
(f_let 4)
(f_let 5)
(type f_let)
(print f_let)
(print "LET_DONE")

;; while
(set f_while (fn (n) (let (i 0 s 0) (while (< i n) (set s (+ s 1)) (set i (+ i 1))) s)))
(f_while 1)
(f_while 2)
(f_while 3)
(f_while 4)
(f_while 5)
(type f_while)
(print f_while)
(print "WHILE_DONE")

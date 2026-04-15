;; 综合验证新版控制流与局部绑定形式在 JIT 下的正确性。
(set fact (fn (n)
  (let (res 1)
    (while (> n 0)
      (set res (* res n))
      (set n (- n 1)))
    res)))

(print "Testing JIT with let, while, set")
(print "fact 5 = " (fact 5))
(fact 5) (fact 5) (fact 5) (fact 5) (fact 5) ;; trigger jit
(cond ((= (fact 5) 120)
       (print "PASS: let/while/set JIT"))
      (#t (print "FAIL: let/while/set JIT")))

(set absolute (fn (x)
  (cond ((< x 0) (- 0 x))
        (#t x))))

(print "Testing JIT with cond")
(print "abs -5 = " (absolute -5))
(print "abs 5 = " (absolute 5))
(absolute -5) (absolute -5) (absolute -5) (absolute -5) (absolute -5) ;; trigger jit
(set res (absolute -5))
(set res2 (absolute 5))
(cond ((= res 5)
       (cond ((= res2 5)
              (print "PASS: cond JIT"))
             (#t (print "FAIL: cond JIT"))))
      (#t (print "FAIL: cond JIT")))

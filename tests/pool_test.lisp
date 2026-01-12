;; Pool lifecycle stress test: allocate many objects and exit

(set i 0)
(while (< i 100)
  (set j 0)
  (while (< j 100)
    ;; create some allocations: numbers, pairs, functions
    (set tmp (list))
    (set k 0)
    (while (< k 100)
      (set tmp (cons (list k) tmp))
      (set k (+ k 1)))
    (set fns (list))
    (set k 0)
    (while (< k 100)
      (set fns (cons (fn (x) (+ x 1)) fns))
      (set k (+ k 1)))
    (set j (+ j 1)))
  (set i (+ i 1)))

(print "pool_test_ok")

;; 内存池生命周期压力测试：大量分配 cons、list 和闭包后直接退出。

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

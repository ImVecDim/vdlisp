(set m (macro (a . rest) (list + a (car rest)))
(m 1 2 3)

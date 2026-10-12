;; coll_alloc.clj - allocation-heavy idiomatic Clojure: each round builds a
;; persistent map of 2,000 entries with assoc and a list of 2,000 elements
;; with cons, then folds both.  pmap runs one task per element of TASKS, all
;; doing the same rounds; every task must return the same checksum.
(defn round [r]
  (let [m (loop [i 0 m {}]
            (if (>= i 2000) m (recur (+ i 1) (assoc m i (+ i r)))))
        v (loop [i 0 v (list)]
            (if (>= i 2000) v (recur (+ i 1) (cons (* i 2) v))))]
    (+ (reduce + (vals m)) (reduce + v) (count m) (count v))))

(defn work [rounds]
  (loop [r 0 acc 0]
    (if (>= r rounds) acc (recur (+ r 1) (+ acc (round r))))))

(def results (vec (pmap (fn [k] (work 1500)) [0 1 2 3 4 5])))
(println "tasks" (count results) "rounds" 1500 "checksum" (first results))
(println (if (= (count (filter (fn [x] (= x (first results))) results)) (count results)) "ok" "FAILED"))

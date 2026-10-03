;; coll_alloc.clj - allocation-heavy idiomatic Clojure: each round builds a
;; persistent map of 2,000 entries with assoc and a vector of 2,000 elements
;; with conj, then folds both.  T futures run the same rounds; every task must
;; return the same checksum.
(defn round [r]
  (let [m (loop [i 0 m {}]
            (if (>= i 2000) m (recur (+ i 1) (assoc m i (+ i r)))))
        v (loop [i 0 v []]
            (if (>= i 2000) v (recur (+ i 1) (conj v (* i 2)))))]
    (+ (reduce + (vals m)) (reduce + v) (count m) (count v))))

(defn work [rounds]
  (loop [r 0 acc 0]
    (if (>= r rounds) acc (recur (+ r 1) (mod (+ (* acc 7) (round r)) 1000000007)))))

(def T (Integer/parseInt (or (System/getenv "T") "1")))
(def R (Integer/parseInt (or (System/getenv "ROUNDS") "100")))
(def results (doall (map deref (doall (map (fn [_] (future (work R))) (range T))))))
(println "tasks" T "rounds" R "checksum" (first results))
(println (if (apply = results) "ok" "FAILED"))

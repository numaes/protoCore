; Control case (write coalescing does not apply to protoClojure): a five-key
; map built with assoc per iteration.
(defn build [i] (assoc {} :a i :b 1 :c 2 :d 3 :e 4))
(defn run [n] (loop [i 0 m nil] (if (< i n) (recur (inc i) (build i)) (inc (:a m)))))
(println "done" (run @N@))

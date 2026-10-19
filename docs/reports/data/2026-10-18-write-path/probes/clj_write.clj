(defn run [] (loop [i 0 m {:x 0 :y 1} s 0] (if (< i 2000000) (let [m2 (assoc m :x i)] (recur (inc i) m2 (+ s (:y m2)))) (+ s (:x m)))))
(println "CHECK" (run))

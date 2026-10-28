(defn run [] (loop [i 0 m {:x 0 :y 1} s 0] (if (< i 2000000) (recur (inc i) m (+ s (:y m))) (+ s (:x m)))))
(println "CHECK" (run))

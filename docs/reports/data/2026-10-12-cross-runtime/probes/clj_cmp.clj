(defn run [] (loop [i 0 s 0] (if (< i 2000000) (recur (inc i) (+ s (if (< i 1000000) 1 0))) s)))
(println "CHECK" (run))

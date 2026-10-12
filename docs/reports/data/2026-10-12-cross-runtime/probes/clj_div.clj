(defn run [] (loop [i 0 s 0] (if (< i 2000000) (recur (inc i) (+ s (/ i 3))) s)))
(println "CHECK" (run))

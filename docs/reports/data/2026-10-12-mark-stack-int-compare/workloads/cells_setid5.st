"Cells per construction: a method with five instance-variable assignments."

Object subclass: #P5 instanceVariableNames: 'id name qty price flag'.

P5 >> setId: i
    id := i. name := 'n'. qty := i + 1. price := 2. flag := true.

P5 >> id
    ^ id

Object subclass: #Keep instanceVariableNames: 'last'.

Keep >> run: n
    1 to: n do: [:i | last := P5 new setId: i].
    ^ last id

Transcript showCr: 'done ' , (Keep new run: @N@) printString.

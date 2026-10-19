Object subclass: #Box instanceVariableNames: 'x y'.

Box >> setX: a y: b
  x := a. y := b.
  ^ self.

Box >> run: n
  | s |
  s := 0.
  1 to: n do: [ :i | x := i. s := s + y ].
  ^ s + x.

b := Box new.
b setX: 0 y: 1.
Transcript showCr: 'CHECK ' , (b run: 2000000) printString.

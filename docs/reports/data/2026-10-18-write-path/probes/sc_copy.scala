case class P(x: Int, y: Int)

@main def run(): Unit =
  var p = P(0, 1)
  var s = 0L
  var i = 0
  while (i < 2000000) {
    p = p.copy(x = i)
    s += p.y
    i += 1
  }
  println("CHECK " + (s + p.x))

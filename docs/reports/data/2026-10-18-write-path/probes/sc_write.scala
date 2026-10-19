class Box(var x: Int, val y: Int)

@main def run(): Unit =
  val b = new Box(0, 1)
  var s = 0L
  var i = 0
  while (i < 2000000) {
    b.x = i
    s += b.y
    i += 1
  }
  println("CHECK " + (s + b.x))

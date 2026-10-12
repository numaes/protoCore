@main def run(): Unit =
  var s = 0L
  var i = 0
  while (i < 2000000) {
    s += i
    i += 1
  }
  println("CHECK " + s)

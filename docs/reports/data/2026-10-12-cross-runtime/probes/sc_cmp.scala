@main def run(): Unit =
  var s = 0L
  var i = 0
  while (i < 2000000) {
    s += (if (i < 1000000) 1 else 0)
    i += 1
  }
  println("CHECK " + s)

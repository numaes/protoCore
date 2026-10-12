// Cells per construction: a case class with five constructor parameter fields.
case class P5(id: Int, name: String, qty: Int, price: Int, flag: Boolean)
@main def run(): Unit =
  var keep = P5(0, "n", 0, 2, true)
  var i = 1
  while i <= @N@ do
    keep = P5(i, "n", i, 2, true)
    i += 1
  println("done " + keep.id)

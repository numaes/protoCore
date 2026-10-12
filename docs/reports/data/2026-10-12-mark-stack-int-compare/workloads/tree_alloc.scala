// tree_alloc.scala - allocation-heavy idiomatic Scala: every round builds a
// complete binary tree of case classes (depth 12, 8191 objects), path-copies
// its leftmost spine and folds both versions. Each batch runs T Futures of
// the same 5 rounds; every Future must return the same checksum (540451801).
sealed trait Tree
case class Leaf(value: Int) extends Tree
case class Node(left: Tree, right: Tree, weight: Int) extends Tree

def build(depth: Int, seed: Int): Tree =
  if depth == 0 then Leaf(seed % 1000)
  else Node(build(depth - 1, seed * 2 + 1), build(depth - 1, seed * 2 + 2), depth)

def bump(t: Tree): Tree = t match
  case Leaf(v)       => Leaf(v + 1)
  case Node(l, r, w) => Node(bump(l), r, w)

def count(t: Tree): Int = t match
  case Leaf(_)       => 1
  case Node(l, r, _) => 1 + count(l) + count(r)

def checksum(t: Tree): Long = t match
  case Leaf(v)       => v
  case Node(l, r, w) => (checksum(l) * 31 + checksum(r) + w) % 1000000007L

def work(rounds: Int): Long =
  var acc = 0L
  var r = 0
  while r < rounds do
    val t = build(12, r)
    val t2 = bump(t)
    acc = (acc * 7 + checksum(t) + checksum(t2) + count(t)) % 1000000007L
    r += 1
  acc

val te = System.getenv("T")
val T = if te == "" then 1 else te.toInt
val be = System.getenv("BATCHES")
val B = if be == "" then 10 else be.toInt
val R = 5
// Rounds run in batches of T Futures of R rounds each: protoScala keeps a
// function's garbage until the function returns, so one long loop per task
// would hold every tree it built.
var first = -1L
var same = true
var b = 0
while b < B do
  var fs: List[Any] = Nil
  var k = 0
  while k < T do
    fs = Future(work(R)) :: fs
    k += 1
  fs.foreach { f =>
    val v = f.await
    if first < 0 then first = v
    if v != first then same = false
  }
  b += 1
println("tasks=" + T.toString + " batches=" + B.toString + " trees=" + (T.toLong * B * R).toString + " checksum=" + first.toString)
println(if same then "ok" else "FAILED")

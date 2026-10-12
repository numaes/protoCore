# Cells per construction: __init__ with five attribute writes.
class P:
    def __init__(self, a, b, c, d, e):
        self.a = a
        self.b = b
        self.c = c
        self.d = d
        self.e = e


N = @N@
last = None
for i in range(N):
    last = P(i, 1, 2, 3, 4)
print("done", last.a + 1)

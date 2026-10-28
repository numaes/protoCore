class Box:
    def __init__(self):
        self.a = 0
        self.b = 1
        self.c = 2
        self.d = 3
        self.e = 4
        self.f = 5
        self.g = 6
        self.x = 0


def run(n):
    b = Box()
    s = 0
    i = 0
    while i < n:
        b.x = i
        s += b.b
        i += 1
    return s + b.x


print("CHECK", run(2000000))

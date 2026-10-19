class Box:
    def __init__(self):
        self.x = 0
        self.y = 1


def run(n):
    b = Box()
    s = 0
    i = 0
    while i < n:
        b.x = i
        s += b.y
        i += 1
    return s + b.x


print("CHECK", run(2000000))

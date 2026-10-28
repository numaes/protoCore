class Box:
    pass


def run(n):
    s = 0
    i = 0
    while i < n:
        b = Box()
        b.x = i
        b.y = 1
        b.z = 2
        s += b.y + b.z
        i += 1
    return s


print("CHECK", run(500000))

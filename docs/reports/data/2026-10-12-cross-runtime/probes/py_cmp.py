n = 2000000
s = 0
i = 0
while i < n:
    s += (1 if i < 1000000 else 0)
    i += 1
print("CHECK", s)

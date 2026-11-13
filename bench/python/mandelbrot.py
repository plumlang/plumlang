inside = 0
for py in range(400):
    y0 = py / 200.0 - 1.0
    for px in range(400):
        x0 = px / 160.0 - 2.0
        x = 0.0
        y = 0.0
        i = 0
        while i < 100 and x * x + y * y <= 4.0:
            t = x * x - y * y + x0
            y = 2.0 * x * y + y0
            x = t
            i += 1
        if i == 100:
            inside += 1
print(inside)

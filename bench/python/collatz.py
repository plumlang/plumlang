best = 0
best_start = 0
for start in range(1, 300000):
    n = start
    steps = 0
    while n != 1:
        if n % 2 == 0:
            n = n // 2
        else:
            n = 3 * n + 1
        steps += 1
    if steps > best:
        best = steps
        best_start = start
print(best_start, best)

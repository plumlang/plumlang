chars = 0
for i in range(500000):
    line = f"item {i}: {i * 2} ({i % 7:>3})"
    chars += len(line)
print(chars)

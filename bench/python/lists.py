# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
def total(xs):
    s = 0
    for x in xs:
        s += x
    return s


def dot(a, b):
    s = 0
    for i in range(len(a)):
        s += a[i] * b[i]
    return s


def column(grid, c):
    s = 0
    for r in range(len(grid)):
        s += grid[r][c]
    return s


def longest(words):
    best = ""
    for w in words:
        if len(w) > len(best):
            best = w
    return best


def with_square(xs, x):
    xs.append(x * x % 1000)
    return xs


xs = []
x = 7
for i in range(200000):
    x = (x * 75 + 74) % 65537
    xs.append(x % 1000)
sums = 0
for k in range(40):
    sums += total(xs) + dot(xs, xs) % 1000

grid = []
for r in range(300):
    row = []
    for c in range(300):
        row.append((r * 31 + c * 17) % 101)
    grid.append(row)
cols = 0
for k in range(40):
    for c in range(300):
        cols += column(grid, c)

syllables = ["ka", "lo", "mi", "nu", "pe", "ra", "si", "to"]
groups = {}
for i in range(100000):
    x = (x * 75 + 74) % 65537
    word = syllables[x % 8] + syllables[x // 8 % 8] + syllables[x // 64 % 8]
    key = syllables[x % 8]
    if key in groups:
        groups[key].append(word)
    else:
        groups[key] = [word]
letters = 0
champion = ""
for key, words in groups.items():
    for k in range(20):
        w = longest(words)
        letters += len(w)
        if w > champion:
            champion = w

built = []
for i in range(3000):
    built = with_square(built, i)

print(sums, cols, len(groups), letters, champion, total(built))

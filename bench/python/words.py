# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
syllables = ["ka", "lo", "mi", "nu", "pe", "ra", "si", "to", "va", "ze", "bo", "du", "fe", "gi", "ho", "ju"]
parts = []
x = 1
for i in range(1000000):
    x = (x * 75 + 74) % 65537
    parts.append(syllables[x % 16] + syllables[x // 16 % 16] + syllables[x // 256 % 16])
text = " ".join(parts)

counts = {}
for word in text.split(" "):
    counts[word] = counts.get(word, 0) + 1
best = ""
best_count = 0
for word, n in counts.items():
    if n > best_count or (n == best_count and word < best):
        best = word
        best_count = n
print(len(counts), best, best_count)

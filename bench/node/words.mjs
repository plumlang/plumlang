// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
const syllables = ["ka", "lo", "mi", "nu", "pe", "ra", "si", "to", "va", "ze", "bo", "du", "fe", "gi", "ho", "ju"];
const parts = [];
let x = 1;
for (let i = 0; i < 1000000; i++) {
  x = (x * 75 + 74) % 65537;
  parts.push(syllables[x % 16] + syllables[Math.floor(x / 16) % 16] + syllables[Math.floor(x / 256) % 16]);
}
const text = parts.join(" ");

const counts = new Map();
for (const word of text.split(" ")) {
  counts.set(word, (counts.get(word) ?? 0) + 1);
}
let best = "";
let bestCount = 0;
for (const [word, n] of counts) {
  if (n > bestCount || (n === bestCount && word < best)) {
    best = word;
    bestCount = n;
  }
}
console.log(counts.size, best, bestCount);

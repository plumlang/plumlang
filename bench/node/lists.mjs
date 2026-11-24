// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
function total(xs) {
  let s = 0;
  for (const x of xs) s += x;
  return s;
}

function dot(a, b) {
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i] * b[i];
  return s;
}

function column(grid, c) {
  let s = 0;
  for (let r = 0; r < grid.length; r++) s += grid[r][c];
  return s;
}

function longest(words) {
  let best = "";
  for (const w of words) {
    if (w.length > best.length) best = w;
  }
  return best;
}

function withSquare(xs, x) {
  xs.push((x * x) % 1000);
  return xs;
}

const xs = [];
let x = 7;
for (let i = 0; i < 200000; i++) {
  x = (x * 75 + 74) % 65537;
  xs.push(x % 1000);
}
let sums = 0;
for (let k = 0; k < 40; k++) sums += total(xs) + (dot(xs, xs) % 1000);

const grid = [];
for (let r = 0; r < 300; r++) {
  const row = [];
  for (let c = 0; c < 300; c++) row.push((r * 31 + c * 17) % 101);
  grid.push(row);
}
let cols = 0;
for (let k = 0; k < 40; k++) {
  for (let c = 0; c < 300; c++) cols += column(grid, c);
}

const syllables = ["ka", "lo", "mi", "nu", "pe", "ra", "si", "to"];
const groups = new Map();
for (let i = 0; i < 100000; i++) {
  x = (x * 75 + 74) % 65537;
  const word = syllables[x % 8] + syllables[Math.floor(x / 8) % 8] + syllables[Math.floor(x / 64) % 8];
  const key = syllables[x % 8];
  if (groups.has(key)) groups.get(key).push(word);
  else groups.set(key, [word]);
}
let letters = 0;
let champion = "";
for (const [key, words] of groups) {
  for (let k = 0; k < 20; k++) {
    const w = longest(words);
    letters += w.length;
    if (w > champion) champion = w;
  }
}

let built = [];
for (let i = 0; i < 3000; i++) built = withSquare(built, i);

console.log(sums, cols, groups.size, letters, champion, total(built));

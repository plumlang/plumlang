// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
let best = 0;
let bestStart = 0;
for (let start = 1; start < 300000; start++) {
  let n = start;
  let steps = 0;
  while (n !== 1) {
    if (n % 2 === 0) {
      n = n / 2;
    } else {
      n = 3 * n + 1;
    }
    steps += 1;
  }
  if (steps > best) {
    best = steps;
    bestStart = start;
  }
}
console.log(bestStart, best);

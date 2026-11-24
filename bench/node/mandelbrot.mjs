// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
let inside = 0;
for (let py = 0; py < 400; py++) {
  const y0 = py / 200.0 - 1.0;
  for (let px = 0; px < 400; px++) {
    const x0 = px / 160.0 - 2.0;
    let x = 0.0;
    let y = 0.0;
    let i = 0;
    while (i < 100 && x * x + y * y <= 4.0) {
      const t = x * x - y * y + x0;
      y = 2.0 * x * y + y0;
      x = t;
      i += 1;
    }
    if (i === 100) inside += 1;
  }
}
console.log(inside);

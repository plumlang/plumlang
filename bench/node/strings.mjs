// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
let chars = 0;
for (let i = 0; i < 500000; i++) {
  const line = `item ${i}: ${i * 2} (${String(i % 7).padStart(3)})`;
  chars += line.length;
}
console.log(chars);

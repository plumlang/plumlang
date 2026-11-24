// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
let total = 0;
for (let i = 0; i < 20000000; i++) {
  if (i % 3 === 0) total += i;
}
console.log(total);

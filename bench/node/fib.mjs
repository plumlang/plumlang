// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
function fib(n) {
  if (n < 2) return n;
  return fib(n - 1) + fib(n - 2);
}
console.log(fib(30));

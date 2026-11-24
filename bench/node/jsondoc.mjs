// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
let total = 0;
let chars = 0;
for (let round = 0; round < 5; round++) {
  const records = [];
  for (let i = 0; i < 20000; i++) {
    records.push({ id: i, name: `user${i}`, active: i % 3 === 0, score: (i * 7 + round) % 1000, tags: ["a", `b${i % 10}`], parent: null });
  }
  const doc = { round, records };
  const text = JSON.stringify(doc);
  chars += text.length;
  const back = JSON.parse(text);
  for (const r of back.records) {
    if (r.active) total += r.score;
  }
  chars += JSON.stringify(back).length;
}
console.log(chars, total);

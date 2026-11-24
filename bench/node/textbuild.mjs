// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
const count = (s, part) => s.split(part).length - 1;
let text = "";
for (let i = 0; i < 20000; i++) {
  text += `row ${i},${(i * 3) % 101},${i % 7}\n`;
}
const pieces = [];
for (let i = 0; i < 200000; i++) {
  pieces.push("<td>");
  pieces.push(`${i % 1000}`);
  pieces.push("</td>");
}
const html = pieces.join("");
console.log(text.length, count(text, "\n"), html.length, count(html, "<td>9"));

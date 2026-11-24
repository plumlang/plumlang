// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
class Circle {
  constructor(r) { this.r = r; }
  area() { return 3.0 * this.r * this.r; }
}
class Rect {
  constructor(w, h) { this.w = w; this.h = h; }
  area() { return this.w * this.h; }
}
class Square extends Rect {
  constructor(side) { super(side, side); }
}
class Tally {
  constructor() { this.total = 0.0; this.count = 0; }
  add(x) {
    this.total += x;
    this.count += 1;
  }
}

const shapes = [new Circle(1.0), new Rect(2.0, 3.0), new Square(2.0)];
const tally = new Tally();
for (let i = 0; i < 1000000; i++) {
  for (const s of shapes) tally.add(s.area());
}
console.log(`${tally.count} ${tally.total.toFixed(1)}`);

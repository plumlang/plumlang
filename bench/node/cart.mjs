// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
class Book {
  constructor(price) { this.price = price; }
  cost(qty) { return this.price * qty; }
}
class Food {
  constructor(price, grams) { this.price = price; this.grams = grams; }
  cost(qty) { return this.price * qty + Math.floor(this.grams / 100); }
}
class Gift {
  constructor(price) { this.price = price; }
  cost(qty) {
    if (qty > 2) return this.price * (qty - 1);
    return this.price * qty;
  }
}
class Ticket extends Book {
  constructor(price) { super(price + 5); }
}
class Stats {
  constructor() { this.total = 0; this.big = 0; }
  add(x) {
    this.total += x;
    if (x > 1000) this.big += 1;
  }
}

const cart = [];
for (let i = 0; i < 2000; i++) {
  const k = i % 4;
  if (k === 0) cart.push(new Book((i % 50) + 1));
  else if (k === 1) cart.push(new Food((i % 30) + 2, i % 900));
  else if (k === 2) cart.push(new Gift((i % 70) + 3));
  else cart.push(new Ticket(i % 40));
}
const stats = new Stats();
for (let round = 0; round < 2000; round++) {
  const qty = (round % 5) + 1;
  for (const item of cart) {
    stats.add(item.cost(qty) * ((round % 13) + 1));
  }
}
console.log(cart.length, stats.total, stats.big);

# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
class Book:
    def __init__(self, price):
        self.price = price

    def cost(self, qty):
        return self.price * qty


class Food:
    def __init__(self, price, grams):
        self.price = price
        self.grams = grams

    def cost(self, qty):
        return self.price * qty + self.grams // 100


class Gift:
    def __init__(self, price):
        self.price = price

    def cost(self, qty):
        if qty > 2:
            return self.price * (qty - 1)
        return self.price * qty


class Ticket(Book):
    def __init__(self, price):
        super().__init__(price + 5)


class Stats:
    def __init__(self):
        self.total = 0
        self.big = 0

    def add(self, x):
        self.total += x
        if x > 1000:
            self.big += 1


cart = []
for i in range(2000):
    k = i % 4
    if k == 0:
        cart.append(Book(i % 50 + 1))
    elif k == 1:
        cart.append(Food(i % 30 + 2, i % 900))
    elif k == 2:
        cart.append(Gift(i % 70 + 3))
    else:
        cart.append(Ticket(i % 40))
stats = Stats()
for round in range(2000):
    qty = round % 5 + 1
    for item in cart:
        stats.add(item.cost(qty) * (round % 13 + 1))
print(len(cart), stats.total, stats.big)

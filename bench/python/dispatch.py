class Circle:
    def __init__(self, r):
        self.r = r

    def area(self):
        return 3.0 * self.r * self.r


class Rect:
    def __init__(self, w, h):
        self.w = w
        self.h = h

    def area(self):
        return self.w * self.h


class Square(Rect):
    def __init__(self, side):
        super().__init__(side, side)


class Tally:
    def __init__(self):
        self.total = 0.0
        self.count = 0

    def add(self, x):
        self.total += x
        self.count += 1


shapes = [Circle(1.0), Rect(2.0, 3.0), Square(2.0)]
tally = Tally()
for i in range(1000000):
    for s in shapes:
        tally.add(s.area())
print(f"{tally.count} {tally.total:.1f}")

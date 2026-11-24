-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local Circle = {}
Circle.__index = Circle
function Circle.new(r) return setmetatable({r = r}, Circle) end
function Circle:area() return 3.0 * self.r * self.r end

local Rect = {}
Rect.__index = Rect
function Rect.new(w, h) return setmetatable({w = w, h = h}, Rect) end
function Rect:area() return self.w * self.h end

local Square = setmetatable({}, {__index = Rect})
Square.__index = Square
function Square.new(side) return setmetatable({w = side, h = side}, Square) end

local Tally = {}
Tally.__index = Tally
function Tally.new() return setmetatable({total = 0.0, count = 0}, Tally) end
function Tally:add(x)
  self.total = self.total + x
  self.count = self.count + 1
end

local shapes = {Circle.new(1.0), Rect.new(2.0, 3.0), Square.new(2.0)}
local tally = Tally.new()
for i = 0, 999999 do
  for _, s in ipairs(shapes) do
    tally:add(s:area())
  end
end
print(string.format("%d %.1f", tally.count, tally.total))

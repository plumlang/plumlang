-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local Book = {}
Book.__index = Book
function Book.new(price) return setmetatable({price = price}, Book) end
function Book:cost(qty) return self.price * qty end

local Food = {}
Food.__index = Food
function Food.new(price, grams) return setmetatable({price = price, grams = grams}, Food) end
function Food:cost(qty) return self.price * qty + self.grams // 100 end

local Gift = {}
Gift.__index = Gift
function Gift.new(price) return setmetatable({price = price}, Gift) end
function Gift:cost(qty)
  if qty > 2 then return self.price * (qty - 1) end
  return self.price * qty
end

local Ticket = setmetatable({}, {__index = Book})
Ticket.__index = Ticket
function Ticket.new(price) return setmetatable({price = price + 5}, Ticket) end

local Stats = {}
Stats.__index = Stats
function Stats.new() return setmetatable({total = 0, big = 0}, Stats) end
function Stats:add(x)
  self.total = self.total + x
  if x > 1000 then self.big = self.big + 1 end
end

local cart = {}
for i = 0, 1999 do
  local k = i % 4
  if k == 0 then cart[#cart + 1] = Book.new(i % 50 + 1)
  elseif k == 1 then cart[#cart + 1] = Food.new(i % 30 + 2, i % 900)
  elseif k == 2 then cart[#cart + 1] = Gift.new(i % 70 + 3)
  else cart[#cart + 1] = Ticket.new(i % 40) end
end
local stats = Stats.new()
for round = 0, 1999 do
  local qty = round % 5 + 1
  for _, item in ipairs(cart) do
    stats:add(item:cost(qty) * (round % 13 + 1))
  end
end
print(#cart .. " " .. stats.total .. " " .. stats.big)

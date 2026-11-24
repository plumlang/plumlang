-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local function total(xs)
  local s = 0
  for _, x in ipairs(xs) do
    s = s + x
  end
  return s
end

local function dot(a, b)
  local s = 0
  for i = 1, #a do
    s = s + a[i] * b[i]
  end
  return s
end

local function column(grid, c)
  local s = 0
  for r = 1, #grid do
    s = s + grid[r][c]
  end
  return s
end

local function longest(words)
  local best = ""
  for _, w in ipairs(words) do
    if #w > #best then
      best = w
    end
  end
  return best
end

local function with_square(xs, x)
  xs[#xs + 1] = x * x % 1000
  return xs
end

local xs = {}
local x = 7
for i = 1, 200000 do
  x = (x * 75 + 74) % 65537
  xs[i] = x % 1000
end
local sums = 0
for k = 1, 40 do
  sums = sums + total(xs) + dot(xs, xs) % 1000
end

local grid = {}
for r = 0, 299 do
  local row = {}
  for c = 0, 299 do
    row[c + 1] = (r * 31 + c * 17) % 101
  end
  grid[r + 1] = row
end
local cols = 0
for k = 1, 40 do
  for c = 1, 300 do
    cols = cols + column(grid, c)
  end
end

local syllables = {"ka", "lo", "mi", "nu", "pe", "ra", "si", "to"}
local groups = {}
local ngroups = 0
for i = 1, 100000 do
  x = (x * 75 + 74) % 65537
  local word = syllables[x % 8 + 1] .. syllables[x // 8 % 8 + 1] .. syllables[x // 64 % 8 + 1]
  local key = syllables[x % 8 + 1]
  local g = groups[key]
  if g then
    g[#g + 1] = word
  else
    groups[key] = {word}
    ngroups = ngroups + 1
  end
end
local letters = 0
local champion = ""
for key, words in pairs(groups) do
  for k = 1, 20 do
    local w = longest(words)
    letters = letters + #w
    if w > champion then
      champion = w
    end
  end
end

local built = {}
for i = 0, 2999 do
  built = with_square(built, i)
end

print(sums .. " " .. cols .. " " .. ngroups .. " " .. letters .. " " .. champion .. " " .. total(built))

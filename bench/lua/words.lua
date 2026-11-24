-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local syllables = {"ka", "lo", "mi", "nu", "pe", "ra", "si", "to", "va", "ze", "bo", "du", "fe", "gi", "ho", "ju"}
local parts = {}
local x = 1
for i = 1, 1000000 do
  x = (x * 75 + 74) % 65537
  parts[i] = syllables[x % 16 + 1] .. syllables[x // 16 % 16 + 1] .. syllables[x // 256 % 16 + 1]
end
local text = table.concat(parts, " ")

local counts = {}
local distinct = 0
for word in string.gmatch(text, "[^ ]+") do
  local n = counts[word]
  if n == nil then
    distinct = distinct + 1
    counts[word] = 1
  else
    counts[word] = n + 1
  end
end
local best = ""
local best_count = 0
for word, n in pairs(counts) do
  if n > best_count or (n == best_count and word < best) then
    best = word
    best_count = n
  end
end
print(distinct .. " " .. best .. " " .. best_count)

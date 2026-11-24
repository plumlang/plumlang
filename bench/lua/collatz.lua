-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local best = 0
local best_start = 0
for start = 1, 299999 do
  local n = start
  local steps = 0
  while n ~= 1 do
    if n % 2 == 0 then
      n = n // 2
    else
      n = 3 * n + 1
    end
    steps = steps + 1
  end
  if steps > best then
    best = steps
    best_start = start
  end
end
print(best_start .. " " .. best)

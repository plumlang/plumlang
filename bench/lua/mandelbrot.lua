-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local inside = 0
for py = 0, 399 do
  local y0 = py / 200.0 - 1.0
  for px = 0, 399 do
    local x0 = px / 160.0 - 2.0
    local x, y, i = 0.0, 0.0, 0
    while i < 100 and x * x + y * y <= 4.0 do
      local t = x * x - y * y + x0
      y = 2.0 * x * y + y0
      x = t
      i = i + 1
    end
    if i == 100 then
      inside = inside + 1
    end
  end
end
print(inside)

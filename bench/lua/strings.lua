-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local chars = 0
for i = 0, 499999 do
  local line = string.format("item %d: %d (%3d)", i, i * 2, i % 7)
  chars = chars + #line
end
print(chars)

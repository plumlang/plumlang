-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local total = 0
for i = 0, 19999999 do
  if i % 3 == 0 then
    total = total + i
  end
end
print(total)

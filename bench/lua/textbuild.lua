-- SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
local function count(s, part)
  local n, at = 0, 1
  while true do
    local i, j = string.find(s, part, at, true)
    if i == nil then return n end
    n = n + 1
    at = j + 1
  end
end

local text = ""
for i = 0, 19999 do
  text = text .. "row " .. i .. "," .. (i * 3 % 101) .. "," .. (i % 7) .. "\n"
end
local pieces = {}
for i = 0, 199999 do
  pieces[#pieces + 1] = "<td>"
  pieces[#pieces + 1] = tostring(i % 1000)
  pieces[#pieces + 1] = "</td>"
end
local html = table.concat(pieces)
print(#text .. " " .. count(text, "\n") .. " " .. #html .. " " .. count(html, "<td>9"))

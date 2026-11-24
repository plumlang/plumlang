# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
text = ""
for i in range(20000):
    text += f"row {i},{i * 3 % 101},{i % 7}\n"
pieces = []
for i in range(200000):
    pieces.append("<td>")
    pieces.append(f"{i % 1000}")
    pieces.append("</td>")
html = "".join(pieces)
print(len(text), text.count("\n"), len(html), html.count("<td>9"))

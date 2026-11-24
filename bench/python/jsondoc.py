# SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
import json

total = 0
chars = 0
for round in range(5):
    records = []
    for i in range(20000):
        records.append({"id": i, "name": f"user{i}", "active": i % 3 == 0, "score": (i * 7 + round) % 1000, "tags": ["a", f"b{i % 10}"], "parent": None})
    doc = {"round": round, "records": records}
    text = json.dumps(doc, separators=(",", ":"))
    chars += len(text)
    back = json.loads(text)
    for r in back["records"]:
        if r["active"]:
            total += r["score"]
    chars += len(json.dumps(back, separators=(",", ":")))
print(chars, total)

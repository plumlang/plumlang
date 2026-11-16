/* SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0 */
/* Fuzz target: any bytes, as JSON text, through json.parse, the Json
 * methods and json.stringify, which must read back as the same value. */
#include "fuzz.h"

static const char driver[] =
    "seed io\n"
    "seed json\n"
    "branch walk(v: Json, depth: int) -> int {\n"
    "    plant n = 1\n"
    "    if depth > 600 { return n }\n"
    "    for k in v.keys() { n += walk(v.get(k), depth + 1) }\n"
    "    if plant items = v.list() {\n"
    "        for i in 0..len(items) { n += walk(v.at(i), depth + 1) }\n"
    "    }\n"
    "    drop(v.type_name(), v.len(), v.is_null(), v.str(), v.int(), v.float(), v.bool())\n"
    "    return n\n"
    "}\n"
    "plant text = io.read_all()\n"
    "try {\n"
    "    plant v = json.parse(text)\n"
    "    walk(v, 0)\n"
    "    plant once = json.stringify(v)\n"
    "    plant pretty = json.stringify(v, indent: 2)\n"
    "    assert(json.stringify(json.parse(once)) == once, \"stringify reads back\")\n"
    "    assert(json.stringify(json.parse(pretty)) == once, \"indented stringify reads back\")\n"
    "} catch e: ParseError {\n"
    "    drop(e.message)\n"
    "}\n";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_driver(driver, data, size, true);
    return 0;
}

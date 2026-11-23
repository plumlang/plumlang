/* SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0 */
/*
 * Fuzz target: any bytes, as what arrives from the network, through the
 * net.http module (written in Plumlang, src/stdlib/plum/net_http.plum):
 * sent to its server as requests, given to its client as a response, and
 * read as a URL and a query string. Over loopback sockets only.
 */
#include "fuzz.h"

static const char driver[] =
    "seed io\n"
    "seed net\n"
    "seed net.http\n"
    "plant input = io.read_all()\n"
    "\n"
    "# the server, reading the input as requests\n"
    "plant app = http.server()\n"
    "app.log_errors = bad\n"
    "app.get(\"/a/:id\", (req) => http.text(\"{req.params} {req.query} {req.headers}\"))\n"
    "app.post(\"/j\", (req) => http.json(req.json()))\n"
    "plant l = net.listen(0, host: \"127.0.0.1\")\n"
    "plant served = spawn (() => app.serve(l.accept()))()\n"
    "plant c = net.connect(\"127.0.0.1\", l.port())\n"
    "c.write(input)\n"
    "c.close()\n"
    "served.wait()\n"
    "l.close()\n"
    "\n"
    "# the client, reading the input as a response\n"
    "plant raw = net.listen(0, host: \"127.0.0.1\")\n"
    "plant answered = spawn (() => {\n"
    "    plant conn = raw.accept()\n"
    "    while (conn.read_line(timeout: 5.0) ?? \"\") != \"\" {}\n"
    "    try { conn.write(input) } catch e: NetError {}\n"
    "    conn.close()\n"
    "})()\n"
    "try {\n"
    "    plant r = http.get(\"http://127.0.0.1:{raw.port()}/\", timeout: 5.0)\n"
    "    drop(r.status, r.ok(), r.header(\"content-type\"), r.body.byte_len(), r)\n"
    "} catch e: NetError {\n"
    "} catch e: ValueError {\n"
    "}\n"
    "answered.wait()\n"
    "raw.close()\n"
    "\n"
    "# a URL and a query string\n"
    "drop(http.parse_query(input))\n"
    "try { drop(http.parse_url(input).host) } catch e: ValueError {}\n";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_driver(driver, data, size, false);
    return 0;
}

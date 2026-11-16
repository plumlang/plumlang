// SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
//
// Run programs with the WebAssembly build, for tests/wasm.sh:
//
//   node tests/wasm/run.mjs build/wasm/plum.js OUTDIR FILE...
//
// One instance runs every FILE in turn (as a playground page does), and
// writes OUTDIR/N.out, N.err and N.code for the Nth file (from 1). An
// API check (check, format, version, streaming) runs first; a failure
// there exits with code 2.
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const [plumJs, outDir, ...files] = process.argv.slice(2);
const { default: createPlum } = await import(pathToFileURL(resolve(plumJs)).href);
const plum = await createPlum();

function fail(what, got) {
  console.error(`wasm: ${what}: ${JSON.stringify(got)}`);
  process.exit(2);
}

// The API, apart from running whole programs.
if (!/^\d+\.\d+\.\d+/.test(plum.version)) fail("version", plum.version);
const diags = plum.check('plant x: int = "a"\ndrop(x)\n');
if (diags.length !== 1 || diags[0].severity !== "error" || diags[0].line !== 1 ||
    diags[0].column !== 16 || !/^E\d{4}$/.test(diags[0].code)) {
  fail("check", diags);
}
if (plum.check("drop(1)\n").length !== 0) fail("check of a good program", plum.check("drop(1)\n"));
const formatted = plum.format("plant   x=1+2\ndrop( x )\n");
if (!formatted.ok || formatted.text !== "plant x = 1 + 2\ndrop(x)\n") fail("format", formatted);
const bad = plum.format("plant = \n");
if (bad.ok || bad.diagnostics.length === 0) fail("format of a syntax error", bad);
const chunks = [];
const streamed = plum.run('drop("a")\ndrop("é🌿")\n', { onStdout: (t) => chunks.push(t) });
if (streamed.exitCode !== 0 || chunks.join("") !== "a\né🌿\n" || chunks.length < 2) {
  fail("streamed output", { streamed, chunks });
}

let n = 0;
for (const file of files) {
  n++;
  const r = plum.run(readFileSync(file, "utf8"));
  writeFileSync(`${outDir}/${n}.out`, r.stdout);
  writeFileSync(`${outDir}/${n}.err`, r.stderr);
  writeFileSync(`${outDir}/${n}.code`, `${r.exitCode}\n`);
}

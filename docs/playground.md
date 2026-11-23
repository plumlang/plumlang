# Plumlang in the browser

`make wasm` builds `plum` for WebAssembly, so a web page can run Plumlang
programs without a server: the playground at
[plum.elixpo.com/playground](https://plum.elixpo.com/playground/) is built
on it. This page is for anyone embedding it in a page of their own.

## From npm

Each release is published to npm as
[`@plumlang/plumlang`](https://www.npmjs.com/package/@plumlang/plumlang):
the same `plum.js` and `plum.wasm`, with TypeScript types, for Node 18+
and for bundlers.

```sh
npm install @plumlang/plumlang
```

```js
import createPlum from "@plumlang/plumlang";
const plum = await createPlum();
```

`plum.js` loads `plum.wasm` with `new URL("plum.wasm", import.meta.url)`,
which bundlers follow; `createPlum({ locateFile: (file) => ... })` points
it elsewhere. Without a bundler, a page can import
`https://cdn.jsdelivr.net/npm/@plumlang/plumlang/plum.js`. The package
is assembled from the release's wasm archive by `tools/release/npm.sh`,
from [`packaging/npm/`](../packaging/npm/), and published by
`.github/workflows/npm.yml` ([releasing.md](releasing.md)).

## Building

```sh
make wasm        # build/wasm/plum.js and build/wasm/plum.wasm
```

It uses Emscripten's `emcc` if it is on the `PATH`, or else the
`emscripten/emsdk` image the Makefile pins (`EMSDK_IMAGE`), through
Docker. Every release also carries the two files as
`plum-X.Y.Z-wasm.tar.gz`, listed in `SHA256SUMS` with a build attestation
like the other archives ([releasing.md](releasing.md)).

| File | What |
|---|---|
| `plum.js` | An ES module (Emscripten's `MODULARIZE` and `EXPORT_ES6`) whose default export creates an instance. |
| `plum.wasm` | The compiler, VM and standard library: about 480 KB, 190 KB gzipped. `plum.js` fetches it from the same folder. |

Serve `plum.wasm` as `application/wasm` so the browser can compile it
while it downloads. No other headers are needed: the build uses no
threads, so no `Cross-Origin-Opener-Policy` / `-Embedder-Policy`. Nothing
is bundled into it (no files, no preload), and memory starts at 32 MB and
grows as needed up to 256 MB.

## The API

```js
import createPlum from "./plum.js";

const plum = await createPlum();

const result = plum.run('drop("Hello, browser!")', {
  onStdout: (text) => output.append(text), // as the program writes it
  onStderr: (text) => output.append(text),
});
// result: { exitCode: 0, stdout: "Hello, browser!\n", stderr: "" }

plum.check('plant x: int = "a"\n');
// [{ severity: "error", code: "E0402", message: "expected int, found str",
//    line: 1, column: 16, endLine: 1, endColumn: 19 }]

plum.format("plant   x=1\n");
// { ok: true, text: "plant x = 1\n" }
// or, with syntax errors: { ok: false, diagnostics: [...] }

plum.version; // the version of plum, such as "0.1.0"
```

**`run(source, options?)`** checks, compiles and runs a program, as
`plum run main.plum` does, and returns `{ exitCode, stdout, stderr }`.
Compile errors and runtime errors go to standard error exactly as the
command line prints them (without colours), naming the program
`main.plum`. Output reaches `onStdout` and `onStderr` as it is written, a
line at a time (or when the program flushes). The options:

| Option | Default | What |
|---|---|---|
| `onStdout`, `onStderr` | none | `(text) => void`, called as output is written. |
| `maxSteps` | 50,000,000 | Calls and backward jumps (loops) before the program is stopped: about a second of work. |
| `maxMemory` | 64 MB | Bytes of values in use before the program is stopped. |
| `maxSleep` | 5 | Seconds the program may spend waiting in `time.sleep`; `false` refuses sleeping. |

**`check(source)`** returns the diagnostics of `plum check` as an array in
source order. Lines and columns count from 1, and columns count UTF-16
code units, as JavaScript strings do, so they index the editor's text
directly. `severity` is `"error"` or `"warning"`; `code` is documented in
[errors.md](errors.md).

**`format(source)`** returns the text `plum fmt` would write, or the
syntax errors that stop it.

Each call is synchronous. An instance can run any number of programs one
after another; nothing carries over between runs. If a call throws (plum
itself failed, which a program should never cause), the instance is
marked `plum.broken` and further calls throw: create a new one.

## What a program can do

Every run is **sandboxed**: the functions that reach outside the page
throw an `IOError` ("isn't allowed in this run (sandbox)"): the `fs`
module, the network (`net`, `net.http`), and `os.run`. There are no other
files, so `seed ./file` and packages report that they can't be found.
TLS is not built in at all (a `NetError` says so).

Everything else works as it does natively: trees, traits, errors, the
standard library's modules that stay inside the program (`math`, `json`,
`hash`, `io`, and `time`'s clocks and dates), and **tasks and channels**, which run on the
VM's own scheduler. `os.platform()` returns `"wasm"`.

**Input**: standard input is always at its end, so `ask()` and
`io.read_line()` return none and `io.read_all()` an empty string.

**Limits**: an endless loop stops at the step limit with
`main.plum: Error: stopped after 50000000 steps, the limit of this run`
and exit code 1, instead of freezing the page; a runaway list stops at the
memory limit the same way.

**Sleeping**: `time.sleep` works, up to `maxSleep` seconds of waiting in
all over a run. Tasks asleep at the same time wait together (a thousand
tasks each sleeping 0.1 s cost 0.1 s), and a sleep that would go past the
limit stops the program with a message. The build has no Asyncify, which
would make every call asynchronous and the module much larger: a sleep
waits on the clock instead, so the thread is busy while it waits. That is
why the playground runs plum in a **Web Worker**, which is what we
recommend for any page:

```js
// worker.js
import createPlum from "./plum.js";
const ready = createPlum(); // load once; messages wait for it
self.onmessage = async ({ data: { id, source } }) => {
  const plum = await ready;
  const result = plum.run(source, {
    onStdout: (text) => self.postMessage({ id, stream: "stdout", text }),
    onStderr: (text) => self.postMessage({ id, stream: "stderr", text }),
  });
  self.postMessage({ id, done: true, exitCode: result.exitCode });
};

// the page
const worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
```

The page stays responsive while a program runs, output arrives as it is
printed, and a Stop button can simply `worker.terminate()` and start a new
worker.

## How it is built

The C sources are the same as the native build's. The pieces that differ:

| File | What |
|---|---|
| `src/wasm/playground.c` | Run, check and format a program held in memory, with the sandbox and limits. Plain C, built into every `plum` and unit-tested natively. |
| `src/wasm/bindings.c` | The exports to JavaScript, and the streams that send output to it (Emscripten only). |
| `src/wasm/api.js` | The JavaScript API above, added to the module with `--post-js`. |
| `src/stdlib/platform.c` | No sockets to poll; sleeping waits on the clock. |
| `src/stdlib/net.c` | Socket calls fail as with no network, which keeps Emscripten's socket and file-system emulation out of the build. |
| `src/stdlib/tls.c` | `PLUM_NO_TLS`: TLS calls fail with a reason. Mbed TLS's crypto stays, for `hash`. |

`tests/wasm.sh` (part of `make test` when a wasm build and Node are
there) runs the examples, the blocks of [reference.md](reference.md) and
the golden tests that need no files or network in one instance under
Node, and checks that each prints exactly what native `plum run` prints;
it also checks that an endless loop stops and that the sandbox holds.

# @plumlang/plumlang

[Plumlang](https://plum.elixpo.com) compiled to WebAssembly: run, check
and format Plumlang programs in a web page or in Node.js, with no server
and no native install. It is the same build the
[playground](https://plum.elixpo.com/playground/) runs on, and the same
compiler, virtual machine and standard library as the `plum` command.

```sh
npm install @plumlang/plumlang
```

The package is an ES module (`plum.js`) and the compiler itself
(`plum.wasm`, about 500 KB, 190 KB gzipped), with TypeScript types. The
full guide is at
[plum.elixpo.com/docs/tools/wasm](https://plum.elixpo.com/docs/tools/wasm/).

## Node.js

```js
import createPlum from "@plumlang/plumlang";

const plum = await createPlum();

const result = plum.run('drop("Hello, Node!")', {
  onStdout: (text) => process.stdout.write(text), // as the program writes it
});
// { exitCode: 0, stdout: "Hello, Node!\n", stderr: "" }

plum.check('plant x: int = "a"\n');
// [{ severity: "error", code: "E0402", message: "expected int, found str",
//    line: 1, column: 16, endLine: 1, endColumn: 19 }]

plum.format("plant   x=1\n");
// { ok: true, text: "plant x = 1\n" }
// or, with syntax errors: { ok: false, diagnostics: [...] }

plum.version; // "2.2.0"
```

Node 18 or newer.

## In the browser

`plum.js` loads `plum.wasm` from next to itself
(`new URL("plum.wasm", import.meta.url)`), which Vite, webpack 5, Rollup
and esbuild-based bundlers follow, so importing the package is enough:

```js
import createPlum from "@plumlang/plumlang";
const plum = await createPlum();
```

Without a bundler, load it from a CDN:

```html
<script type="module">
  import createPlum from "https://cdn.jsdelivr.net/npm/@plumlang/plumlang/plum.js";
  const plum = await createPlum();
  console.log(plum.run('drop("Hello, browser!")').stdout);
</script>
```

If you serve `plum.wasm` from somewhere else, say where:

```js
const plum = await createPlum({ locateFile: (file) => `/assets/${file}` });
```

Serve `plum.wasm` as `application/wasm`. No other headers are needed (no
threads, so no cross-origin isolation).

## The API

| | |
|---|---|
| `run(source, options?)` | Checks, compiles and runs a program as `plum run main.plum` does. Returns `{ exitCode, stdout, stderr }`. Options: `onStdout`, `onStderr` (called as output is written), `maxSteps` (default 50,000,000), `maxMemory` (bytes, default 64 MB), `maxSleep` (seconds of `time.sleep`, default 5; `false` refuses sleeping). |
| `check(source)` | The diagnostics of `plum check`, in source order: `{ severity, code, message, line, column, endLine, endColumn }`. Lines and columns count from 1; columns count UTF-16 code units, so they index a JavaScript string directly. |
| `format(source)` | `{ ok: true, text }`, the text `plum fmt` would write, or `{ ok: false, diagnostics }`. |
| `version` | The version of plum. |

Every call is synchronous. An instance runs any number of programs one
after another, and nothing carries over between runs. If a call throws
(plum itself failed, which a program should never cause), the instance is
marked `broken`: create a new one.

Every run is sandboxed: `fs`, the network and `os.run` throw an `IOError`,
standard input is empty, and an endless loop or a runaway list stops at
the step or memory limit with a message instead of hanging.

## Long-running programs: use a worker

`run` blocks the thread until the program ends, and `time.sleep` waits on
the clock. On a page, run plum in a Web Worker so the page stays
responsive, output arrives as it is printed, and a Stop button can end a
run with `worker.terminate()`:

```js
// worker.js
import createPlum from "@plumlang/plumlang";
const ready = createPlum(); // load once; messages wait for it

self.onmessage = async ({ data: { id, source } }) => {
  const plum = await ready;
  const result = plum.run(source, {
    onStdout: (text) => self.postMessage({ id, stream: "stdout", text }),
    onStderr: (text) => self.postMessage({ id, stream: "stderr", text }),
  });
  self.postMessage({ id, done: true, exitCode: result.exitCode });
};
```

```js
// the page
let worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
worker.onmessage = ({ data }) => {
  if (data.done) console.log("exit code", data.exitCode);
  else output.append(data.text);
};
worker.postMessage({ id: 1, source: 'drop("Hello from a worker")' });

// Stop: end the worker and start a fresh one
stop.onclick = () => {
  worker.terminate();
  worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
};
```

In Node, the same works with `worker_threads`.

## Versions

The package's version is the version of Plumlang it was built from. It is
published from each [release](https://github.com/plumlang/plumlang/releases),
whose `plum-X.Y.Z-wasm.tar.gz` holds the same two files.

## Licence

Apache-2.0 ([LICENSE](LICENSE)), or the Plumlang Commercial License for
holders of a written agreement with Elixpo. The names and logos are
covered by Plumlang's
[trademark rules](https://github.com/plumlang/plumlang/blob/main/TRADEMARKS.md).

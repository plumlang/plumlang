// SPDX-License-Identifier: LicenseRef-Plumlang-Commercial-1.0
//
// The JavaScript API of the WebAssembly build (docs/playground.md), added
// to the module object by `emcc --post-js`. It runs inside the module's
// factory, where `Module` and the runtime's helpers are in scope.
//
//   import createPlum from "./plum.js";
//   const plum = await createPlum();
//   plum.run('drop("hi")', { onStdout: (text) => ... });  // { exitCode, stdout, stderr }
//   plum.check(source);   // [{ severity, code, message, line, column, endLine, endColumn }]
//   plum.format(source);  // { ok: true, text } or { ok: false, diagnostics }
//   plum.version;         // "0.1.0"

(function () {
  "use strict";

  // The run in progress: where its output goes.
  var current = null;

  Module["_plumEmit"] = function (stream, bytes) {
    if (current === null) {
      return;
    }
    var text = (stream === 1 ? current.outDecoder : current.errDecoder).decode(bytes, {
      stream: true,
    });
    current.write(stream, text);
  };

  // Copy a string into the module's memory: [pointer, length in bytes].
  function toMemory(text) {
    text = String(text);
    var len = lengthBytesUTF8(text);
    var ptr = _malloc(len + 1);
    stringToUTF8(text, ptr, len + 1);
    return [ptr, len];
  }

  // Call `fn(ptr, len, ...rest)` with `source` in memory; free it after.
  function withSource(source, fn) {
    if (Module["broken"]) {
      throw new Error("plum: this instance stopped after an internal error; create a new one");
    }
    var mem = toMemory(source);
    try {
      return fn(mem[0], mem[1]);
    } catch (e) {
      // An abort inside plum (out of memory, an internal error): the
      // instance can't be trusted any more.
      Module["broken"] = true;
      throw e;
    } finally {
      if (!Module["broken"]) {
        _free(mem[0]);
      }
    }
  }

  // A JSON string returned by plum, parsed, and the string freed.
  function takeJson(ptr) {
    var text = UTF8ToString(ptr);
    _plum_wasm_free(ptr);
    return JSON.parse(text);
  }

  function limit(value) {
    return typeof value === "number" && isFinite(value) ? value : 0;
  }

  Module["run"] = function (source, options) {
    options = options || {};
    var result = { exitCode: 1, stdout: "", stderr: "" };
    var run = {
      outDecoder: new TextDecoder(),
      errDecoder: new TextDecoder(),
      write: function (stream, text) {
        if (text === "") {
          return;
        }
        if (stream === 1) {
          result.stdout += text;
          if (options.onStdout) options.onStdout(text);
        } else {
          result.stderr += text;
          if (options.onStderr) options.onStderr(text);
        }
      },
    };
    var before = current;
    current = run;
    try {
      result.exitCode = withSource(source, function (ptr, len) {
        return _plum_wasm_run(
          ptr,
          len,
          limit(options.maxSteps),
          limit(options.maxMemory),
          options.maxSleep === false ? -1 : limit(options.maxSleep),
        );
      });
      run.write(1, run.outDecoder.decode());
      run.write(2, run.errDecoder.decode());
    } finally {
      current = before;
    }
    return result;
  };

  Module["check"] = function (source) {
    return withSource(source, function (ptr, len) {
      return takeJson(_plum_wasm_check(ptr, len));
    });
  };

  Module["format"] = function (source) {
    return withSource(source, function (ptr, len) {
      return takeJson(_plum_wasm_format(ptr, len));
    });
  };

  Object.defineProperty(Module, "version", {
    get: function () {
      return UTF8ToString(_plum_wasm_version());
    },
  });
})();

// SPDX-License-Identifier: Apache-2.0 OR LicenseRef-Plumlang-Commercial-1.0
//
// Types for the JavaScript API of Plumlang's WebAssembly build
// (docs/playground.md, src/wasm/api.js).

/** A compile error or warning, as `plum check` reports it. */
export interface Diagnostic {
  severity: "error" | "warning";
  /** The error code, such as `"E0402"` (docs/errors.md). */
  code: string;
  message: string;
  /** Lines and columns count from 1; columns count UTF-16 code units. */
  line: number;
  column: number;
  endLine: number;
  endColumn: number;
}

/** What `run` returns once the program has finished. */
export interface RunResult {
  /** 0 on success; 1 for a compile error, a runtime error or a limit. */
  exitCode: number;
  /** Everything the program wrote to standard output. */
  stdout: string;
  /** Everything written to standard error: errors as `plum run` prints them, without colours. */
  stderr: string;
}

export interface RunOptions {
  /** Called with output as the program writes it, a line at a time or on flush. */
  onStdout?: (text: string) => void;
  /** Called with error output as it is written. */
  onStderr?: (text: string) => void;
  /** Calls and backward jumps before the program is stopped. Default 50,000,000. */
  maxSteps?: number;
  /** Bytes of values in use before the program is stopped. Default 64 MB. */
  maxMemory?: number;
  /** Seconds the program may spend in `time.sleep`; `false` refuses sleeping. Default 5. */
  maxSleep?: number | false;
}

/** What `format` returns: the formatted text, or the syntax errors that stop it. */
export type FormatResult =
  | { ok: true; text: string }
  | { ok: false; diagnostics: Diagnostic[] };

/** An instance of plum. Every call is synchronous; nothing carries over between runs. */
export interface Plum {
  /** Check, compile and run a program, as `plum run main.plum` does. */
  run(source: string, options?: RunOptions): RunResult;
  /** The diagnostics of `plum check`, in source order. */
  check(source: string): Diagnostic[];
  /** The text `plum fmt` would write, or the syntax errors that stop it. */
  format(source: string): FormatResult;
  /** The version of plum, such as `"2.2.0"`. */
  readonly version: string;
  /** True once a call has thrown (plum itself failed): create a new instance. */
  readonly broken?: boolean;
}

/** Options passed through to the Emscripten module factory. */
export interface PlumModuleOptions {
  /** Where to find `plum.wasm`, if not next to `plum.js`. */
  locateFile?: (path: string, prefix: string) => string;
  [key: string]: unknown;
}

/** Load plum.wasm and create an instance. */
declare function createPlum(options?: PlumModuleOptions): Promise<Plum>;
export default createPlum;

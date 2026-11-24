// makefun-site, faster: a native engine with a pure-JS fallback.
// run(input) returns makefun-site 1.0.0's answer for one case's input,
// byte for byte. The native engine is tried first; anything that stops
// it loading or answering (wrong CPU, missing file) falls back to the
// JavaScript engine in fallback.js, which implements the same semantics.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
let native = null;
try {
  native = require('./engine.node');
} catch {
  native = null;
}

import { run as jsRun } from './fallback.js';

export function run(input) {
  if (native) {
    try {
      return JSON.parse(native.build(JSON.stringify(input)));
    } catch {
      /* fall through to the JavaScript engine */
    }
  }
  return jsRun(input);
}

// The whole cases.json at once: one parse, one answer per case, no round
// trip through JS values. Takes the raw bytes (a Buffer, or a string) and
// returns the answers as a Buffer. Used by main.ts when the native engine
// is present; main.ts falls back to run() per case otherwise.
export function runAll(casesInput) {
  if (!native) return null;
  try {
    return native.runAll(casesInput);
  } catch {
    return null;
  }
}

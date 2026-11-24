// The harness runs `node /bot/main.ts` (node22). It reads every case from
// /input/cases.json (a JSON array of {id, input}), answers each one alone
// with run() from solve.js, and writes /output/answers.json: a JSON array
// of the answers, one per case and in the cases' order. Outside a
// container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for
// /input and /output.
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

// V8's incremental marking off, set before solve.js is imported so all of
// its work runs under it (the house baseline runs with
// --no-incremental-marking, and an entry may tune V8's flags too).
setFlagsFromString('--no-incremental-marking');

const mod = await import(new URL('./solve.js', import.meta.url).href);

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const text = readFileSync(join(INPUT_DIR, 'cases.json'));

// Fast path: the native engine answers all cases from the raw bytes in
// one call, taking a Buffer in and returning a Buffer out (no string
// copies either way). If it is unavailable, answer each case through
// run() instead.
let answers = null;
if (typeof mod.runAll === 'function') answers = mod.runAll(text);
if (answers == null) {
  const cases = JSON.parse(text.toString('utf8'));
  const list = [];
  for (const c of cases) list.push(mod.run(c.input));
  answers = JSON.stringify(list);
}
writeFileSync(join(OUTPUT_DIR, 'answers.json'), answers);

# site-generator-speed-01 v2 — a native makefun-site engine

[![make.fun](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/portgas-commits/site-generator-speed-01-v2/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/portgas-commits/site-generator-speed-01-v2/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122)

A faster drop-in for [makefun-site 1.0.0](https://make.fun/bounty/site-generator-speed-01-run-20261122):
`run(input)` returns makefun-site's answer for one generated site, byte for
byte — every page's `index.html`, index and tag pages, `build-report.txt`,
and every static file — then speed is measured in CPU time.

## How it works

- **`src/engine.c` + `src/build.c` + friends** — a native N-API engine
  written in C. One `build(json)` call parses a case, runs the whole
  pipeline (front matter, permalinks, Markdown, collections, tags,
  layouts, report) in a single arena that is dropped when the case is
  done, and serialises the output tree. Nothing is cached from one case
  to the next. The engine is built from the C source in this repo.
- **`engine.node`** — the compiled addon (built with `gcc -O3`, generic
  x86-64, no `-march=native`: the grading CPU is unknown). It is the only
  binary in the repo, and it is built from `src/` — see "Rebuilding" below.
- **`solve.js`** — the entry point. It loads the native engine and answers
  through it; if the addon cannot load, or a call fails, it falls back to
  the pure-JavaScript engine in `fallback.js`, which implements the same
  semantics. `runAll` answers a whole `cases.json` in one call, taking a
  Buffer in and returning a Buffer out, so no string copies are paid.
- **`fallback.js`** — the pure-JS engine. Correctness first: it is
  byte-identical to the reference on the practice set and on fuzz cases.
- **`main.ts`** — the harness driver: reads `/input/cases.json`, answers
  each case alone, writes `/output/answers.json`. Sets
  `--no-incremental-marking`, as the house baseline does.

## Results (practice set, 3 cases, this machine, Node v22)

| engine | median CPU, 5 runs | vs house baseline |
| --- | --- | --- |
| house makefun-site 1.0.0 | 1.435 s | 1× |
| this entry, native path | 0.277 s | ~5.2× gross, ~7.5× net of the do-nothing control (~0.10 s) |
| this entry, JS fallback | 1.334 s | ~1.1× net — correctness insurance, not the racing path |

The JS fallback is deliberately written for correctness, not speed: it exists
so the entry still answers byte-identically if the native addon cannot load
(a different CPU, a missing file). It is not expected to win on speed.

Correctness: **3/3 practice cases byte-identical** (1,135 output files,
0 differing bytes), `grade-local` reproduced byte for byte, and both
engines clean on fuzz cases generated with the kit's own generator
(2000 cases, 0 differences — see `REPORT.md` in the build folder, not in
this repo).

## Rebuilding the addon

The addon needs the Node 22 headers (the same Node the graders run):

```sh
gcc -O3 -fPIC -shared -I<path-to-node-22>/include/node src/engine.c -o engine.node
```

No other build step, no dependencies, nothing installed before the program
runs. The program itself needs only `node /bot/main.ts`.

## Layout

- `solve.js` — entry: native engine first, JS fallback second
- `fallback.js` — pure-JavaScript engine (same semantics)
- `main.ts` — harness driver
- `package.json` — `{"type": "module"}`
- `src/engine.c` — N-API binding (`build`, `runAll`)
- `src/build.c` — the build pipeline (sources, permalinks, Markdown,
  collections, tags, layouts, report)
- `src/text.c` — slug rule, HTML escaping, long dates
- `src/frontmatter.c` — front-matter parser
- `src/markdown.c` — Markdown block lexer + inline renderer
- `src/template.c` — template tokenizer, parser, renderer
- `src/json.h` — JSON parse/serialise
- `src/common.h` — chunked arena, string buffer, UTF-8, string map
- `src/nfkd_table.h` — NFKD decompositions to ASCII (for slugs)

## License

MIT. See `LICENSE`.

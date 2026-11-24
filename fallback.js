// Pure-JavaScript engine for makefun-site 1.0.0 — the fallback when the
// native engine cannot load (wrong CPU, missing file). It implements the
// same semantics as the native engine: front matter, the Markdown subset,
// the template dialect, slugs, permalinks, collections, tags, layouts and
// the build report, byte for byte. Correctness first; it is not the fast
// path, but it must never produce a wrong byte.

/* ------------------------------------------------------------ text */

// JS \s (what the reference's regexes use): space, tab, newline, CR, FF,
// VT, NBSP, and the Unicode spaces.
function isSpace(ch) {
  return ch === ' ' || ch === '\t' || ch === '\n' || ch === '\r' || ch === '\f' ||
    ch === '\v' || ch === '\u00a0' || ch === '\u1680' ||
    (ch >= '\u2000' && ch <= '\u200a') || ch === '\u2028' || ch === '\u2029' ||
    ch === '\u202f' || ch === '\u205f' || ch === '\u3000' || ch === '\ufeff';
}

function isCombining(ch) {
  const cp = ch.codePointAt(0);
  return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x0483 && cp <= 0x0489) ||
    (cp >= 0x0591 && cp <= 0x05bd) || (cp >= 0x0610 && cp <= 0x061a) ||
    (cp >= 0x064b && cp <= 0x065f) || cp === 0x0670 ||
    (cp >= 0x06d6 && cp <= 0x06dc) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
    (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
    (cp >= 0xfe00 && cp <= 0xfe0f) || (cp >= 0xfe20 && cp <= 0xfe2f);
}

export function slugify(text) {
  // NFKD, drop combining marks, lowercase A-Z, runs of other characters
  // become one '-' BETWEEN alphanumeric runs (never leading or trailing),
  // cut to 64 chars with a '-' left at the cut dropped, 'untitled' when
  // nothing is left.
  const norm = String(text).normalize('NFKD');
  let out = '';
  let gap = false;
  for (const ch of norm) {
    if (isCombining(ch)) continue;
    const cp = ch.codePointAt(0);
    let keep = 0;
    if (cp >= 65 && cp <= 90) keep = cp + 32;
    else if ((cp >= 97 && cp <= 122) || (cp >= 48 && cp <= 57)) keep = cp;
    if (!keep) {
      if (out.length > 0) gap = true;
      continue;
    }
    if (gap) {
      out += '-';
      gap = false;
    }
    out += String.fromCharCode(keep);
  }
  if (out.length > 64) {
    out = out.slice(0, 64).replace(/-+$/, '');
  }
  return out === '' ? 'untitled' : out;
}

function uniqueSlug(slug, taken) {
  if (!taken.has(slug)) {
    taken.add(slug);
    return slug;
  }
  let n = 2;
  for (;;) {
    const cand = slug + '-' + n;
    if (!taken.has(cand)) {
      taken.add(cand);
      return cand;
    }
    n++;
  }
}

function escapeHtml(s) {
  let out = '';
  let start = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    let rep = null;
    if (c === '&') rep = '&amp;';
    else if (c === '<') rep = '&lt;';
    else if (c === '>') rep = '&gt;';
    else if (c === '"') rep = '&quot;';
    else if (c === "'") rep = '&#39;';
    if (rep !== null) {
      out += s.slice(start, i) + rep;
      start = i + 1;
    }
  }
  return out + s.slice(start);
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June',
  'July', 'August', 'September', 'October', 'November', 'December'];
function longDate(date) {
  // '2026-11-09' -> '9 November 2026'
  const y = Number(date.slice(0, 4));
  const m = Number(date.slice(5, 7));
  const d = Number(date.slice(8, 10));
  return d + ' ' + MONTHS[m - 1] + ' ' + y;
}

/* ------------------------------------------------------------ front matter */

const FM_KEYS = ['title', 'date', 'summary', 'tags', 'draft', 'layout', 'slug'];

function isLeap(y) {
  return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0;
}

function parseFmValue(key, raw) {
  // Returns {value} or {error}. Mirrors the reference exactly, including
  // every error string (they go into the build report).
  if (key === 'title' || key === 'summary') {
    if (raw.length > 0 && raw[0] === '"') {
      if (raw.length < 2 || raw[raw.length - 1] !== '"')
        return { error: 'key "' + key + '": a quoted string is not closed' };
      let out = '';
      for (let i = 1; i + 1 < raw.length; i++) {
        const c = raw[i];
        if (c === '\\') {
          const nx = raw[i + 1];
          if ((nx !== '"' && nx !== '\\') || i + 1 === raw.length - 1)
            return { error: 'key "' + key + '": a quoted string holds an escape other than \\" or \\\\' };
          out += nx;
          i++;
        } else if (c === '"') {
          return { error: 'key "' + key + '": a quoted string holds a stray quote' };
        } else {
          out += c;
        }
      }
      return { value: out };
    }
    if (raw.length > 0 && raw[0] === '[')
      return { error: 'key "' + key + '": a bare string opens on [' };
    return { value: raw };
  }
  if (key === 'date') {
    const bad = 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)';
    if (raw.length !== 10 || raw[4] !== '-' || raw[7] !== '-') return { error: bad };
    for (let i = 0; i < 10; i++) {
      if (i === 4 || i === 7) continue;
      if (raw[i] < '0' || raw[i] > '9') return { error: bad };
    }
    const y = Number(raw.slice(0, 4)), mo = Number(raw.slice(5, 7)), d = Number(raw.slice(8, 10));
    const dim = [31, isLeap(y) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > dim[mo - 1])
      return { error: bad };
    return { value: raw };
  }
  if (key === 'tags') {
    if (raw.length < 2 || raw[0] !== '[' || raw[raw.length - 1] !== ']')
      return { error: 'key "tags": not a list [a, b]' };
    let i0 = 1, i1 = raw.length - 1;
    while (i0 < i1 && (raw[i0] === ' ' || raw[i0] === '\t')) i0++;
    while (i1 > i0 && (raw[i1 - 1] === ' ' || raw[i1 - 1] === '\t')) i1--;
    if (i0 >= i1) return { value: [] };
    const tags = [];
    let start = 1;
    for (let i = 1; ; i++) {
      if (i === raw.length - 1 || raw[i] === ',') {
        let s = start, e = i;
        while (s < e && (raw[s] === ' ' || raw[s] === '\t')) s++;
        while (e > s && (raw[e - 1] === ' ' || raw[e - 1] === '\t')) e--;
        tags.push(raw.slice(s, e));
        start = i + 1;
        if (i === raw.length - 1) break;
      }
    }
    for (const t of tags) {
      if (t === '' || t.includes('"') || t.includes('[') || t.includes(']'))
        return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
    }
    return { value: tags };
  }
  if (key === 'draft') {
    if (raw === 'true') return { value: true };
    if (raw === 'false') return { value: false };
    return { error: 'key "draft": true or false' };
  }
  if (key === 'layout') {
    if (raw.length === 0 || !/^[a-z0-9-]+$/.test(raw))
      return { error: 'key "layout": not a template name' };
    return { value: raw };
  }
  if (key === 'slug') {
    if (raw.length === 0 || raw.length > 64 || !/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(raw))
      return { error: 'key "slug": not a slug' };
    return { value: raw };
  }
  return { error: 'unknown key "' + key + '"' };
}

function parseFrontMatter(text) {
  // Returns {pairs: Map, body} or {error}.
  const lines = text.split('\n');
  if (lines.length === 0 || lines[0] !== '---') return { error: 'no front matter' };
  let close = -1;
  for (let i = 1; i < lines.length; i++) {
    if (lines[i] === '---') { close = i; break; }
  }
  if (close < 0) return { error: 'the front matter is not closed' };
  const pairs = new Map();
  for (let i = 1; i < close; i++) {
    const line = lines[i];
    // blank (only spaces/tabs) or a comment: skipped
    if (/^[ \t]*$/.test(line)) continue;
    if (line.length > 0 && line[0] === '#') continue;
    // LINE = /^([a-z_]+):(?: (.*))?$/
    let k = 0;
    while (k < line.length && ((line[k] >= 'a' && line[k] <= 'z') || line[k] === '_')) k++;
    if (k === 0 || k >= line.length || line[k] !== ':')
      return { error: 'line ' + (i + 1) + ' is not "key: value"' };
    const key = line.slice(0, k);
    let raw;
    if (k + 1 < line.length && line[k + 1] === ' ') {
      raw = line.slice(k + 2);
    } else if (k + 1 === line.length) {
      raw = '';
    } else {
      return { error: 'line ' + (i + 1) + ' is not "key: value"' };
    }
    // trailing spaces and tabs dropped
    raw = raw.replace(/[ \t]+$/, '');
    if (!FM_KEYS.includes(key)) return { error: 'unknown key "' + key + '"' };
    if (pairs.has(key)) return { error: 'key "' + key + '" is given twice' };
    if (raw === '') return { error: 'key "' + key + '" has no value' };
    const r = parseFmValue(key, raw);
    if (r.error) return { error: r.error };
    pairs.set(key, r.value);
  }
  if (!pairs.has('title')) return { error: 'no title' };
  const body = lines.slice(close + 1).join('\n');
  return { pairs, body };
}

/* ------------------------------------------------------------ markdown */

function trimLine(s) {
  return s.replace(/^[ \t]+/, '').replace(/[ \t]+$/, '');
}
function isBlankLine(s) {
  return /^[ \t]*$/.test(s);
}
function fenceOpen(line) {
  if (line.length < 3 || line[0] !== '`' || line[1] !== '`' || line[2] !== '`') return null;
  let i = 3;
  while (i < line.length && /[A-Za-z0-9_+-]/.test(line[i])) i++;
  let j = i;
  while (j < line.length && (line[j] === ' ' || line[j] === '\t')) j++;
  if (j !== line.length) return null;
  return line.slice(3, i);
}
function isFenceClose(line) {
  if (line.length < 3 || line[0] !== '`' || line[1] !== '`' || line[2] !== '`') return false;
  for (let i = 3; i < line.length; i++)
    if (line[i] !== ' ' && line[i] !== '\t') return false;
  return true;
}
function headingOf(line) {
  let h = 0;
  while (h < line.length && h < 6 && line[h] === '#') h++;
  if (h === 0 || h > 6) return null;
  if (h >= line.length || (line[h] !== ' ' && line[h] !== '\t')) return null;
  let i = h;
  while (i < line.length && (line[i] === ' ' || line[i] === '\t')) i++;
  let e = line.length;
  while (e > i && (line[e - 1] === ' ' || line[e - 1] === '\t')) e--;
  if (e === i) return null;
  return { depth: h, text: line.slice(i, e) };
}
function isHr(line) {
  if (line.length < 3) return false;
  const dash = line[0] === '-' && line[1] === '-' && line[2] === '-';
  const star = line[0] === '*' && line[1] === '*' && line[2] === '*';
  if (!dash && !star) return false;
  for (let i = 3; i < line.length; i++)
    if (line[i] !== ' ' && line[i] !== '\t') return false;
  return true;
}
function endsParagraph(line) {
  if (isBlankLine(line)) return true;
  if (fenceOpen(line) !== null) return true;
  if (headingOf(line) !== null) return true;
  if (isHr(line)) return true;
  return false;
}

function lexMarkdown(src) {
  const lines = src.split('\n');
  const out = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (isBlankLine(line)) { i++; continue; }
    const lang = fenceOpen(line);
    if (lang !== null) {
      i++;
      const body = [];
      while (i < lines.length && !isFenceClose(lines[i])) body.push(lines[i++]);
      i++; // skip the closing fence, or run past the end
      out.push({ type: 'code', lang, lines: body });
      continue;
    }
    const hd = headingOf(line);
    if (hd) {
      out.push({ type: 'heading', depth: hd.depth, text: hd.text });
      i++;
      continue;
    }
    if (isHr(line)) {
      out.push({ type: 'hr' });
      i++;
      continue;
    }
    if (line.length > 0 && line[0] === '>') {
      const body = [];
      while (i < lines.length && lines[i].length > 0 && lines[i][0] === '>') {
        let l = lines[i].slice(1);
        if (l.length > 0 && l[0] === ' ') l = l.slice(1);
        body.push(l);
        i++;
      }
      const kept = [];
      for (const l of body) {
        const t = trimLine(l);
        if (t.length > 0) kept.push(t);
      }
      out.push({ type: 'quote', lines: kept });
      continue;
    }
    const isBullet = line.length >= 2 && (line[0] === '-' || line[0] === '*') && line[1] === ' ';
    let isOrdered = false, ordStart = 0;
    if (!isBullet && line.length >= 3) {
      const m = /^(\d{1,9})\. (.*)$/.exec(line);
      if (m) { isOrdered = true; ordStart = Number(m[1]); }
    }
    if (isBullet || isOrdered) {
      const items = [];
      while (i < lines.length) {
        const l = lines[i];
        let match = false, item = null;
        if (isOrdered) {
          const m = /^(\d{1,9})\. (.*)$/.exec(l);
          if (m) { match = true; item = m[2]; }
        } else {
          if (l.length >= 2 && (l[0] === '-' || l[0] === '*') && l[1] === ' ') {
            match = true;
            item = l.slice(2);
          }
        }
        if (match) {
          items.push(trimLine(item));
          i++;
        } else {
          // CONTINUED = /^ {2,}\S/ : two or more spaces, then a non-space.
          let sp = 0;
          while (sp < l.length && l[sp] === ' ') sp++;
          let cont = false;
          if (sp >= 2 && sp < l.length) cont = !isSpace(l[sp]);
          if (!cont) break;
          items[items.length - 1] += '\n' + trimLine(l);
          i++;
        }
      }
      out.push({ type: 'list', ordered: isOrdered, start: isOrdered ? ordStart : 1, items });
      continue;
    }
    // paragraph
    let text = trimLine(line);
    i++;
    while (i < lines.length && !endsParagraph(lines[i])) {
      text += '\n' + trimLine(lines[i]);
      i++;
    }
    out.push({ type: 'para', text });
  }
  return out;
}

/* ------------------------------------------------------------ inline */

const ESCAPABLE = new Set(['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']);

function findUnescaped(s, what, from, end) {
  for (let j = from; j + what.length <= end; j++) {
    if (s[j] === '\\' && j + 1 < end && ESCAPABLE.has(s[j + 1])) {
      j++;
      continue;
    }
    if (s.slice(j, j + what.length) === what) return j;
  }
  return -1;
}

function findSingleStar(s, from, end) {
  for (let j = from; j < end; j++) {
    const c = s[j];
    if (c === '\\' && j + 1 < end && ESCAPABLE.has(s[j + 1])) {
      j++;
      continue;
    }
    if (c !== '*') continue;
    if (j + 1 < end && s[j + 1] === '*') {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}

// Render inline markup of s[start..end) into the parts array.
function mdInline(parts, s, start, end, linkFn) {
  let i = start;
  while (i < end) {
    const c = s[i];
    if (c === '\\' && i + 1 < end && ESCAPABLE.has(s[i + 1])) {
      parts.push(escapeHtml(s[i + 1]));
      i += 2;
      continue;
    }
    if (c === '`') {
      // plain indexOf for the closer (an escaped backtick still closes)
      let j = -1;
      for (let k = i + 1; k < end; k++) {
        if (s[k] === '`') { j = k; break; }
      }
      if (j > i + 1) {
        parts.push('<code>' + escapeHtml(s.slice(i + 1, j)) + '</code>');
        i = j + 1;
      } else {
        parts.push('`');
        i++;
      }
      continue;
    }
    if (c === '*') {
      if (i + 1 < end && s[i + 1] === '*') {
        const j = findUnescaped(s, '**', i + 2, end);
        if (j > i + 2) {
          parts.push('<strong>');
          mdInline(parts, s, i + 2, j, linkFn);
          parts.push('</strong>');
          i = j + 2;
        } else {
          parts.push('**');
          i += 2;
        }
        continue;
      }
      const j = findSingleStar(s, i + 1, end);
      if (j > i + 1) {
        parts.push('<em>');
        mdInline(parts, s, i + 1, j, linkFn);
        parts.push('</em>');
        i = j + 1;
      } else {
        parts.push('*');
        i++;
      }
      continue;
    }
    if (c === '[') {
      const k = findUnescaped(s, ']', i + 1, end);
      let m = -1;
      if (k !== -1 && k + 1 < end && s[k + 1] === '(') {
        for (let q = k + 2; q < end; q++) {
          if (s[q] === ')') { m = q; break; }
        }
      }
      let ok = false;
      if (m !== -1) {
        const target = s.slice(k + 2, m);
        if (target.length > 0) {
          ok = true;
          for (const ch of target) {
            if (isSpace(ch)) { ok = false; break; }
          }
        }
      }
      if (ok) {
        const target = s.slice(k + 2, m);
        const href = linkFn(target);
        parts.push('<a href="' + escapeHtml(href) + '">');
        mdInline(parts, s, i + 1, k, linkFn);
        parts.push('</a>');
        i = m + 1;
        continue;
      }
      parts.push('[');
      i++;
      continue;
    }
    parts.push(escapeHtml(c));
    i++;
  }
}

/* ------------------------------------------------------------ templates */

const INCLUDE_DEPTH = 8;

function splitPath(text) {
  return text.split('.');
}
function validPathText(s) {
  return /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/.test(s);
}
function tplErr(name, reason) {
  return 'template "' + name + '": ' + reason;
}

function tokenize(name, src) {
  // Returns {tokens} or {error}.
  const tokens = [];
  let text = '';
  let i = 0;
  const len = src.length;
  const flushText = () => {
    if (text.length > 0) {
      tokens.push({ kind: 'text', text });
      text = '';
    }
  };
  while (i < len) {
    let open = -1;
    for (let k = i; k < len; k++) {
      if (src[k] === '{') { open = k; break; }
    }
    if (open === -1 || open === len - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const closer = raw ? '}}}' : '}}';
      const close = src.indexOf(closer, open + (raw ? 3 : 2));
      if (close === -1) {
        return { error: tplErr(name, raw ? 'a {{{ is not closed' : 'a {{ is not closed') };
      }
      flushText();
      let ps = open + (raw ? 3 : 2);
      let pe = close;
      while (ps < pe && (src[ps] === ' ' || src[ps] === '\t' || src[ps] === '\n' || src[ps] === '\r')) ps++;
      while (pe > ps && (src[pe - 1] === ' ' || src[pe - 1] === '\t' || src[pe - 1] === '\n' || src[pe - 1] === '\r')) pe--;
      const pathText = src.slice(ps, pe);
      if (!validPathText(pathText)) {
        const quoted = '"' + pathText.replace(/["\\]/g, (c) => '\\' + c) + '"';
        return { error: tplErr(name, quoted + ' is not a path') };
      }
      tokens.push({ kind: raw ? 'raw' : 'var', path: splitPath(pathText) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    // {% %}
    const close = src.indexOf('%}', open + 2);
    if (close === -1) {
      return { error: tplErr(name, 'a {% is not closed') };
    }
    const end = close + 2;
    // standalone: only spaces/tabs beside it on its line
    let lineStart = open;
    while (lineStart > 0 && src[lineStart - 1] !== '\n') lineStart--;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? len : nl;
    const standalone =
      /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) {
      // drop the text back to the line start
      text = text.slice(0, text.length - (open - lineStart));
    }
    flushText();
    let ts = open + 2, te = close;
    while (ts < te && (src[ts] === ' ' || src[ts] === '\t' || src[ts] === '\n' || src[ts] === '\r')) ts++;
    while (te > ts && (src[te - 1] === ' ' || src[te - 1] === '\t' || src[te - 1] === '\n' || src[te - 1] === '\r')) te--;
    tokens.push({ kind: 'tag', tag: src.slice(ts, te) });
    i = standalone ? (nl === -1 ? len : nl + 1) : end;
  }
  flushText();
  return { tokens };
}

function parseForTag(tag) {
  if (!tag.startsWith('for ')) return null;
  const p = tag.slice(4);
  const m = /^([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/.exec(p);
  if (!m) return null;
  return { varName: m[1], path: splitPath(m[2]) };
}
function parseIfTag(tag) {
  if (!tag.startsWith('if ')) return null;
  const p = tag.slice(3);
  const m = /^if (not )?(\S+)$/.exec(tag);
  if (!m) return null;
  return { not: m[1] === 'not ', path: splitPath(m[2]) };
}
function parseIncludeTag(tag) {
  const m = /^include "([a-z0-9-]+)"$/.exec(tag);
  if (!m) return null;
  return m[1];
}

function parseTemplate(name, tokens) {
  // Returns {nodes} or {error}. Frames hold the parent list and the index
  // of the open node (not a pointer into it).
  const root = [];
  let cur = root;
  const frames = []; // {parent, index, body, inElse}
  for (const tok of tokens) {
    if (tok.kind === 'text') {
      cur.push({ kind: 'text', text: tok.text });
    } else if (tok.kind === 'var' || tok.kind === 'raw') {
      cur.push({ kind: tok.kind, path: tok.path });
    } else {
      const tag = tok.tag;
      const f = parseForTag(tag);
      if (f) {
        const nd = { kind: 'for', varName: f.varName, path: f.path, body: [] };
        cur.push(nd);
        frames.push({ parent: cur, index: cur.length - 1, body: [], inElse: false });
        cur = frames[frames.length - 1].body;
        nd.body = cur;
        continue;
      }
      const it = parseIfTag(tag);
      if (it) {
        const nd = { kind: 'if', not: it.not, path: it.path, then: [], else: null };
        cur.push(nd);
        frames.push({ parent: cur, index: cur.length - 1, body: [], inElse: false });
        cur = frames[frames.length - 1].body;
        nd.then = cur;
        continue;
      }
      if (tag === 'else') {
        const fr = frames[frames.length - 1];
        if (!fr || fr.parent[fr.index].kind !== 'if' || fr.inElse)
          return { error: tplErr(name, 'an else outside an if') };
        fr.parent[fr.index].then = fr.body;
        const elseBody = [];
        fr.parent[fr.index].else = elseBody;
        fr.body = elseBody;
        fr.inElse = true;
        cur = elseBody;
        continue;
      }
      if (tag === 'endfor' || tag === 'endif') {
        const want = tag === 'endfor' ? 'for' : 'if';
        const fr = frames[frames.length - 1];
        if (!fr || fr.parent[fr.index].kind !== want)
          return { error: tplErr(name, 'an ' + tag + ' with no open ' + want) };
        frames.pop();
        const nd = fr.parent[fr.index];
        if (nd.kind === 'for') nd.body = fr.body;
        else if (fr.inElse) nd.else = fr.body;
        else nd.then = fr.body;
        cur = frames.length === 0 ? root : frames[frames.length - 1].body;
        continue;
      }
      const inc = parseIncludeTag(tag);
      if (inc !== null) {
        cur.push({ kind: 'include', name: inc });
        continue;
      }
      return { error: tplErr(name, '{% ' + tag + ' %} is not a tag') };
    }
  }
  if (frames.length > 0) {
    const fr = frames[frames.length - 1];
    const kind = fr.parent[fr.index].kind;
    return { error: tplErr(name, 'a ' + kind + ' is not closed') };
  }
  return { nodes: root };
}

// Lookup: walk the scope chain for the first frame whose vars own path[0],
// then walk the path through nested objects.
function lookupPath(scope, path) {
  let s = scope;
  while (s && !Object.prototype.hasOwnProperty.call(s.vars, path[0])) s = s.parent;
  if (!s) return undefined;
  let v = s.vars[path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || Array.isArray(v)) return undefined;
    if (!Object.prototype.hasOwnProperty.call(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

function numToString(d) {
  if (Number.isInteger(d)) return String(d);
  // %.17g approximation: 17 significant digits, trailing zeros stripped,
  // exponent form when the exponent is < -4 or >= 17.
  let s = d.toPrecision(17);
  if (s.includes('e')) {
    const [mant, exp] = s.split('e');
    const m = mant.replace(/\.?0+$/, '');
    return m + 'e' + (Number(exp) >= 0 ? '+' : '-') + String(Math.abs(Number(exp))).padStart(2, '0');
  }
  if (s.includes('.')) s = s.replace(/\.?0+$/, '');
  return s;
}

// A value as text: string itself, number in JS digits, true/false, array
// items joined by ", ", anything else "".
function textOf(v) {
  if (v === undefined || v === null) return '';
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return numToString(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

function truthy(v) {
  if (v === undefined || v === null) return false;
  if (Array.isArray(v)) return v.length > 0;
  if (typeof v === 'boolean') return v;
  if (typeof v === 'string') return v.length > 0;
  if (typeof v === 'number') return v !== 0;
  return true; // objects (incl. empty) are truthy
}

function renderNodes(parts, nodes, scope, templates, depth) {
  for (const nd of nodes) {
    if (nd.kind === 'text') {
      parts.push(nd.text);
    } else if (nd.kind === 'var') {
      parts.push(escapeHtml(textOf(lookupPath(scope, nd.path))));
    } else if (nd.kind === 'raw') {
      parts.push(textOf(lookupPath(scope, nd.path)));
    } else if (nd.kind === 'if') {
      const tv = truthy(lookupPath(scope, nd.path));
      const branch = tv !== nd.not ? nd.then : nd.else;
      if (branch) renderNodes(parts, branch, scope, templates, depth);
    } else if (nd.kind === 'for') {
      const xs = lookupPath(scope, nd.path);
      if (!Array.isArray(xs)) continue;
      for (let k = 0; k < xs.length; k++) {
        const vars = {
          [nd.varName]: xs[k],
          loop: { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length },
        };
        renderNodes(parts, nd.body, { vars, parent: scope }, templates, depth);
      }
    } else if (nd.kind === 'include') {
      const t = templates.get(nd.name);
      if (t && depth < INCLUDE_DEPTH) renderNodes(parts, t.nodes, scope, templates, depth + 1);
    }
  }
}

/* ------------------------------------------------------------ pipeline */

const RESERVED = ['posts', 'tags', 'page'];

function dirOf(path) {
  const i = path.lastIndexOf('/');
  return i < 0 ? '' : path.slice(0, i);
}

function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else {
      parts.push(seg);
    }
  }
  return parts.join('/');
}

function wordsOf(body) {
  const m = body.match(/\S+/g);
  return m ? m.length : 0;
}

function build(input) {
  const report = [];
  const output = new Map(); // path -> text, insertion order
  const outHas = (p) => output.has(p);
  const outPut = (p, t) => output.set(p, t);

  const siteIn = input.site || {};
  const base = String(siteIn.base_url || '').replace(/\/+$/, '');

  // Templates, parsed in code-unit order of name.
  const templates = new Map();
  const tmplIn = input.templates || {};
  for (const name of Object.keys(tmplIn).sort()) {
    const text = tmplIn[name];
    const tk = tokenize(name, typeof text === 'string' ? text : '');
    if (tk.error) {
      report.push(tk.error);
      continue;
    }
    const pr = parseTemplate(name, tk.tokens);
    if (pr.error) {
      report.push(pr.error);
      continue;
    }
    templates.set(name, pr);
  }

  // Sources, sorted by path.
  const files = (input.files || []).slice().sort((a, b) =>
    a.path < b.path ? -1 : a.path > b.path ? 1 : 0);
  const sources = [];
  const statics = [];
  for (const f of files) {
    const path = f.path || '';
    const content = f.content || '';
    if (path.endsWith('.md')) {
      const text = content.replace(/\r\n?/g, '\n');
      sources.push({ path, text });
    } else {
      statics.push({ path, content });
    }
  }
  const staticPaths = new Set(statics.map((s) => s.path));

  // Front matter + checks, in order.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if (fm.error) {
      report.push('refused ' + s.path + ': ' + fm.error);
      continue;
    }
    s.fm = fm;
    s.post = s.path.startsWith('posts/');
    let own = null;
    for (const d of RESERVED) {
      if (d === 'posts') continue;
      if (s.path.startsWith(d + '/')) { own = d; break; }
    }
    if (own) {
      report.push('refused ' + s.path + ': ' + own + '/ is the generator\'s own');
      continue;
    }
    if (s.post && !fm.pairs.has('date')) {
      report.push('refused ' + s.path + ': a post needs a date');
      continue;
    }
    const layout = fm.pairs.has('layout') ? fm.pairs.get('layout') : (s.post ? 'post' : 'page');
    if (!templates.has(layout)) {
      report.push('refused ' + s.path + ': layout "' + layout + '" is no template that parses');
      continue;
    }
    s.layout = layout;
    if (fm.pairs.get('draft') === true) {
      report.push('draft ' + s.path);
      continue;
    }
    kept.push(s);
  }

  // Permalinks.
  const takenByDir = new Map(); // dir -> Set
  for (const s of kept) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    let taken = takenByDir.get(dir);
    if (!taken) {
      taken = new Set();
      if (dir === '') for (const d of RESERVED) taken.add(d);
      takenByDir.set(dir, taken);
    }
    const baseSlug = fmHas(s, 'slug') ? s.fm.pairs.get('slug') : slugify(s.fm.pairs.get('title'));
    s.slug = uniqueSlug(baseSlug, taken);
    s.outDir = dir === '' ? s.slug : dir + '/' + s.slug;
    s.url = base + '/' + s.outDir + '/';
  }
  const bySource = new Map(kept.map((s) => [s.path, s]));

  // Markdown.
  const broken = new Set();
  for (const s of kept) {
    const dir = dirOf(s.path);
    const linkFn = (target) => {
      if (target[0] === '/' || target[0] === '#') return target;
      const m = /^[A-Za-z][A-Za-z0-9+.-]*:/.exec(target);
      if (m) return target;
      const hash = target.indexOf('#');
      const rel = hash === -1 ? target : target.slice(0, hash);
      const frag = hash === -1 ? '' : target.slice(hash);
      const path = rel === '' ? null : resolvePath(dir, rel);
      if (path) {
        const hit = bySource.get(path);
        if (hit) return hit.url + frag;
        if (staticPaths.has(path)) return base + '/' + path + frag;
      }
      broken.add('broken-link ' + s.path + ' -> ' + target);
      return target;
    };
    const blocks = lexMarkdown(s.fm.body);
    const ids = new Set();
    const parts = [];
    const toc = [];
    let excerpt = null;
    for (const t of blocks) {
      if (t.type === 'heading') {
        const id = uniqueSlug(slugify(t.text), ids);
        const inner = [];
        mdInline(inner, t.text, 0, t.text.length, linkFn);
        const html = inner.join('');
        parts.push('<h' + t.depth + ' id="' + id + '">' + html + '</h' + t.depth + '>\n');
        if (t.depth === 2 || t.depth === 3) toc.push({ level: t.depth, id, html });
      } else if (t.type === 'code') {
        let h = '<pre><code';
        if (t.lang) h += ' class="language-' + escapeHtml(t.lang) + '"';
        h += '>';
        for (const line of t.lines) h += escapeHtml(line) + '\n';
        h += '</code></pre>\n';
        parts.push(h);
      } else if (t.type === 'hr') {
        parts.push('<hr>\n');
      } else if (t.type === 'quote') {
        if (t.lines.length === 0) {
          parts.push('<blockquote>\n</blockquote>\n');
        } else {
          const joined = t.lines.join('\n');
          const inner = [];
          mdInline(inner, joined, 0, joined.length, linkFn);
          parts.push('<blockquote>\n<p>' + inner.join('') + '</p>\n</blockquote>\n');
        }
      } else if (t.type === 'list') {
        let h = t.ordered
          ? (t.start !== 1 ? '<ol start="' + t.start + '">' : '<ol>')
          : '<ul>';
        h += '\n';
        for (const item of t.items) {
          const inner = [];
          mdInline(inner, item, 0, item.length, linkFn);
          h += '<li>' + inner.join('') + '</li>\n';
        }
        h += t.ordered ? '</ol>\n' : '</ul>\n';
        parts.push(h);
      } else if (t.type === 'para') {
        const inner = [];
        mdInline(inner, t.text, 0, t.text.length, linkFn);
        const html = inner.join('');
        if (excerpt === null) excerpt = html;
        parts.push('<p>' + html + '</p>\n');
      }
    }
    s.html = parts.join('');
    s.toc = toc;
    s.excerpt = excerpt === null ? '' : excerpt;
  }

  // Collections.
  const posts = kept.filter((s) => s.post).sort((x, y) => {
    const dx = x.fm.pairs.get('date'), dy = y.fm.pairs.get('date');
    if (dy !== dx) return dy < dx ? -1 : 1;
    return x.slug < y.slug ? -1 : x.slug > y.slug ? 1 : 0;
  });
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }

  // Tags.
  const bySlug = new Map();
  function ownTags(s) {
    const tags = s.fm.pairs.get('tags');
    if (!tags || tags.length === 0) return [];
    const seen = new Set();
    const names = [];
    for (const name of tags) {
      const slug = slugify(name);
      if (seen.has(slug)) continue;
      seen.add(slug);
      names.push(name);
    }
    return names;
  }
  function tagFor(name) {
    const slug = slugify(name);
    let tag = bySlug.get(slug);
    if (!tag) {
      tag = { name, slug, url: base + '/tags/' + slug + '/', posts: [] };
      bySlug.set(slug, tag);
    }
    return tag;
  }
  for (const p of posts) {
    p.tagRefs = [];
    for (const name of ownTags(p)) {
      const tag = tagFor(name);
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of kept) {
    if (s.post) continue;
    s.tagRefs = [];
    for (const name of ownTags(s)) {
      // A page's tag is NOT added to the tag collection: only tags that
      // appear on a post get a tag page and a place in site.tags. A
      // page-only tag is a transient {name, slug, url: ''} object.
      const slug = slugify(name);
      let tag = bySlug.get(slug);
      if (!tag) tag = { name, slug, url: '', posts: [] };
      s.tagRefs.push(tag);
    }
  }
  const tags = [...bySlug.values()].sort((x, y) => {
    if (x.posts.length !== y.posts.length) return y.posts.length - x.posts.length;
    return x.slug < y.slug ? -1 : x.slug > y.slug ? 1 : 0;
  });

  // Cards and pages.
  function cardOf(s) {
    const w = wordsOf(s.fm.body);
    let rm = Math.floor((w + 199) / 200);
    if (rm < 1) rm = 1;
    return {
      title: s.fm.pairs.get('title') || '',
      url: s.url,
      date: s.fm.pairs.get('date') || '',
      date_long: s.fm.pairs.has('date') ? longDate(s.fm.pairs.get('date')) : '',
      summary: s.fm.pairs.get('summary') || '',
      excerpt: s.excerpt,
      reading_minutes: rm,
      tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
    };
  }
  function linkOf(s) {
    if (!s) return null;
    return {
      title: s.fm.pairs.get('title') || '',
      url: s.url,
      date: s.fm.pairs.get('date') || '',
      date_long: s.fm.pairs.has('date') ? longDate(s.fm.pairs.get('date')) : '',
    };
  }
  function pageOf(s) {
    const page = cardOf(s);
    page.slug = s.slug;
    page.content = s.html;
    page.toc = s.toc.map((t) => ({ level: t.level, id: t.id, html: t.html }));
    page.newer = linkOf(s.newer);
    page.older = linkOf(s.older);
    page.is_post = s.post;
    return page;
  }
  function tagOf(t) {
    return {
      name: t.name,
      slug: t.slug,
      url: t.url,
      count: t.posts.length,
      posts: t.posts.map(cardOf),
    };
  }

  // Layouts.
  const site = { title: siteIn.title || '', base_url: base };
  const allTags = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recentN = typeof siteIn.recent === 'number' ? Math.max(0, siteIn.recent) : 0;
  const recent = posts.slice(0, recentN).map(cardOf);
  const baseVars = { site, tags: allTags, recent };
  function renderInto(outPath, tmplName, extraVars) {
    const t = templates.get(tmplName);
    if (!t) return;
    const parts = [];
    renderNodes(parts, t.nodes, { vars: extraVars, parent: { vars: baseVars, parent: null } }, templates, 0);
    outPut(outPath, parts.join(''));
  }

  for (const s of kept) {
    renderInto(s.outDir + '/index.html', s.layout, { page: pageOf(s) });
  }
  const per = typeof siteIn.per_page === 'number' ? siteIn.per_page : 0;
  let pages = 1;
  if (per >= 1) pages = Math.max(1, Math.ceil(posts.length / per));
  for (let k = 1; k <= pages; k++) {
    const from = (k - 1) * per;
    const to = k * per;
    const pagePosts = per < 1 ? [] : posts.slice(from, to).map(cardOf);
    const indexUrl = (n) => (n === 1 ? base + '/' : base + '/page/' + n + '/');
    const pager = {
      number: k,
      total: pages,
      url: indexUrl(k),
      newer_url: k === 1 ? '' : indexUrl(k - 1),
      older_url: k === pages ? '' : indexUrl(k + 1),
    };
    const path = k === 1 ? 'index.html' : 'page/' + k + '/index.html';
    renderInto(path, 'index', { posts: pagePosts, pager });
  }
  for (const t of tags) {
    renderInto('tags/' + t.slug + '/index.html', 'tag', { tag: tagOf(t) });
  }
  renderInto('tags/index.html', 'tags', {});
  for (const st of statics) {
    if (outHas(st.path)) {
      report.push('shadowed ' + st.path + ': a built page has its path');
    } else {
      outPut(st.path, st.content);
    }
  }

  // Report.
  const lines = [...report, ...broken].sort();
  let npages = 0;
  for (const s of kept) if (!s.post) npages++;
  lines.push('built ' + npages + ' pages, ' + posts.length + ' posts, ' + tags.length +
    ' tags and ' + statics.length + ' static files');
  outPut('build-report.txt', lines.join('\n') + '\n');

  // Output object, keys sorted.
  const out = {};
  for (const k of [...output.keys()].sort()) out[k] = output.get(k);
  return out;
}

function fmHas(s, key) {
  return s.fm.pairs.has(key);
}

export function run(input) {
  return build(input);
}

// Bundles the console into one self-contained HTML file for the firmware to
// serve out of flash.
//
// Output is committed at web/dist/console.html on purpose. The firmware build
// runs on a machine with Python and PlatformIO and nothing else -- CI included
// -- so `pio run` must never need npm. Treating the bundle like a vendored
// artifact keeps a fresh clone buildable; `bun run build` regenerates it and CI
// fails the PR if the committed copy has drifted from the source.
//
// Everything is inlined (no external <script>/<link>) because the device serves
// exactly one route for the page and every extra request is another socket the
// single-threaded HTTP loop has to service.

import { build } from 'esbuild';
import { readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const SRC = resolve(HERE, 'src');
const OUT = resolve(HERE, 'dist', 'console.html');

const js = await build({
  entryPoints: [resolve(SRC, 'main.ts')],
  bundle: true,
  format: 'iife',
  target: ['es2020'],
  minify: true,
  write: false,
  legalComments: 'none',
});
const script = js.outputFiles[0].text.trim();

const css = readFileSync(resolve(SRC, 'console.css'), 'utf8');
const body = readFileSync(resolve(SRC, 'body.html'), 'utf8').trim();

// Cheap, safe CSS minification: strip comments and collapse the whitespace the
// authored file uses for readability. Deliberately not a full CSS parser --
// this only has to handle the one stylesheet in this repo.
// No regex for the whitespace pass. Both /\s*\n\s*/ and the [ \t]*\n[ \t]*
// that first replaced it are quadratic: a leading unbounded quantifier with a
// literal after it re-tries at every position in a run of spaces that has no
// newline, and the authored stylesheet is full of those. Trimming line by line
// is linear, does the same job, and does not have to be reasoned about.
const cssMin = css
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .split('\n')
  .map((line) => line.trim())
  .filter((line) => line !== '')
  .join('\n')
  .trim();

const html = `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#0b0e13">
<link rel="manifest" href="/manifest.json">
<link rel="icon" href="/icon.svg">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>Garage fan</title>
<link rel="stylesheet" media="print" onload="this.media='all'" href="https://fonts.googleapis.com/css2?family=Instrument+Sans:wght@400;500;600;700&family=JetBrains+Mono:wght@400;500;600;700&display=swap">
<style>
${cssMin}
</style></head><body>
${body}
<script>
${script}
</script></body></html>
`;

// No content restrictions apply: gen_web_page.py embeds this file as a
// gzipped byte array, so no byte sequence can escape into the C++ source.
mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, html);

const kb = (n) => `${(n / 1024).toFixed(1)} KB`;
console.log(
  `console.html  ${kb(Buffer.byteLength(html))}  ` +
    `(css ${kb(Buffer.byteLength(cssMin))}, js ${kb(Buffer.byteLength(script))}, ` +
    `markup ${kb(Buffer.byteLength(body))})`,
);

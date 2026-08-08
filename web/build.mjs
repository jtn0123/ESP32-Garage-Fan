// Bundles the console into one self-contained HTML file for the firmware to
// serve out of flash.
//
// Output is committed at web/dist/console.html on purpose. The firmware build
// runs on a machine with Python and PlatformIO and nothing else -- CI included
// -- so `pio run` must never need npm. Treating the bundle like a vendored
// artifact keeps a fresh clone buildable; `npm run build` regenerates it and CI
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

// The bundle must not contain the raw string `)html"`, which would terminate
// the C++ raw string literal the page is embedded in and produce a baffling
// compile error hundreds of lines away.
const RAW_STRING_TERMINATOR = ')html"';

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
const cssMin = css
  .replace(/\/\*[\s\S]*?\*\//g, '')
  .replace(/\s*\n\s*/g, '\n')
  .replace(/\n+/g, '\n')
  .trim();

const html = `<!doctype html>
<html><head><meta charset="utf-8">
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

if (html.includes(RAW_STRING_TERMINATOR)) {
  throw new Error(
    `bundle contains ${RAW_STRING_TERMINATOR}, which would terminate the C++ raw string literal in fan_controller_main.cpp`,
  );
}

mkdirSync(dirname(OUT), { recursive: true });
writeFileSync(OUT, html);

const kb = (n) => `${(n / 1024).toFixed(1)} KB`;
console.log(
  `console.html  ${kb(Buffer.byteLength(html))}  ` +
    `(css ${kb(Buffer.byteLength(cssMin))}, js ${kb(Buffer.byteLength(script))}, ` +
    `markup ${kb(Buffer.byteLength(body))})`,
);

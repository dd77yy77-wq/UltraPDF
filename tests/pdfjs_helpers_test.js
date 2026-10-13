// Runs the pdf.js helper functions of index.html against the real bundled pdf.js and a real PDF (tests/fixture.pdf).
const fs = require('fs');
const path = require('path');
const root = path.join(__dirname, '..');
const html = fs.readFileSync(path.join(root, 'assets/index.html'), 'utf8');
const m = html.match(/\/\*PDFJS-HELPERS-BEGIN\*\/([\s\S]*?)\/\*PDFJS-HELPERS-END\*\//);
if (!m) { console.log('helpers markers not found'); process.exit(1); }
const H = new Function(m[1] + '; return { pdfResolveDest, pdfOutline, pdfLinks, pdfText };')();
const pdfjs = require(path.join(root, 'assets/pdfjs/pdf.js'));
pdfjs.GlobalWorkerOptions.workerSrc = path.join(root, 'assets/pdfjs/pdf.worker.js');

let failed = 0; const ok = (c, msg) => { console.log((c ? '  PASS ' : '  FAIL ') + msg); if (!c) failed++; };
(async () => {
  const data = new Uint8Array(fs.readFileSync(path.join(__dirname, 'fixture.pdf')));
  const doc = await pdfjs.getDocument({ data, useSystemFonts: false, verbosity: 0 }).promise;
  ok(doc.numPages === 3, 'document has 3 pages');

  console.log('== outline');
  const o = await H.pdfOutline(doc);
  ok(o.length === 2 && o[0].title.indexOf('Intro') > 0 && o[1].title.indexOf('Target') > 0, 'two top-level entries: ' + o.map(x => x.title).join(' | '));
  ok(o[0].items.length === 1 && o[0].items[0].title.indexOf('1.1') > 0, 'nested entry under the first chapter');
  const pages = [];
  for (const it of [o[0], o[0].items[0], o[1]]) pages.push(await H.pdfResolveDest(doc, it.dest));
  ok(JSON.stringify(pages) === '[0,1,2]', 'outline destinations resolve to page indexes 0,1,2: ' + JSON.stringify(pages));
  ok((await H.pdfResolveDest(doc, 'no-such-dest')) === -1, 'unknown destination -> -1');
  ok((await H.pdfResolveDest(doc, null)) === -1, 'null destination -> -1');

  console.log('== links');
  const links = await H.pdfLinks(doc, 0);
  ok(links.length === 2, 'two links on page 1: ' + links.length);
  const internal = links.find(l => l.dest), external = links.find(l => l.url);
  ok(internal && (await H.pdfResolveDest(doc, internal.dest)) === 2, 'internal link points to page 3');
  ok(external && external.url === 'https://example.com/', 'external link url: ' + (external && external.url));
  ok(Math.abs(internal.vw - 595.28) < 1 && Math.abs(internal.vh - 841.89) < 1, 'viewport is A4 in points: ' + internal.vw + 'x' + internal.vh);
  // link at PDF rect (72, H-145, 190, H-130) -> top-left coordinates y from the top: 130..145
  ok(Math.abs(internal.r[0] - 72) < 1 && Math.abs(internal.r[1] - 130) < 1 && Math.abs(internal.r[2] - 190) < 1 && Math.abs(internal.r[3] - 145) < 1, 'rect converted to top-left origin: ' + internal.r.map(Math.round));
  ok((await H.pdfLinks(doc, 1)).length === 0, 'no links on page 2');

  console.log('== text');
  const t1 = await H.pdfText(doc, 0);
  ok(/Hello UltraPDF first page/.test(t1) && /Go to page three/.test(t1), 'page 1 text: ' + JSON.stringify(t1));
  const t3 = await H.pdfText(doc, 2);
  ok(/Third page/.test(t3) && t3.split('\n').length >= 2, 'page 3 has two lines: ' + JSON.stringify(t3));
  console.log('\nFAILED: ' + failed); process.exit(failed ? 1 : 0);
})().catch(e => { console.log('ERROR', e); process.exit(1); });

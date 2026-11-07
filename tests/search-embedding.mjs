import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, readdir, realpath, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createServer } from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { pdf, stream } from './pdf-fixture.mjs';

const [preparer, indexer, searcher, model] = process.argv.slice(2);
assert.ok(model, 'provide the existing native model directory as the fourth argument');
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-embedding-test-')));
const archives = join(root, 'archives'), prepared = join(root, 'prepared');
const index = join(root, 'index.ops'), embeddings = join(root, 'embeddings');
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const origin = 'https://www.potsdam.sitzung-online.de/oparl/';
const id = path => origin + path, schema = 'https://schema.oparl.org/1.1/';
const run = (binary, args, expected = 0) => {
  const result = spawnSync(binary, args, { encoding: 'utf8', timeout: 180000, maxBuffer: 8 * 1024 * 1024 });
  assert.equal(result.status, expected, `${result.error ?? ''}\n${result.stderr}\n${result.stdout}`);
  return result;
};
const buildArgs = ['--input', archives, '--prepared', prepared, '--output', index,
  '--embeddings', embeddings, '--model', model, '--threads', '2'];
const query = (text, mode = 'semantic', options = []) => JSON.parse(run(searcher,
  ['--index', index, '--embeddings', embeddings, '--model', model, '--threads', '2',
    '--mode', mode, '--group', 'paper', '--json', ...options, text]).stdout);
let child;
try {
  for (const year of [2022, 2026]) {
    const directory = join(archives, String(year));
    await mkdir(join(directory, 'metadata'), { recursive: true });
    await mkdir(join(directory, 'documents'));
    await writeFile(join(directory, 'archive.json'), JSON.stringify({ complete: true, referenceYear: year }));
  }
  const calendar = join(archives, 'calendar-window');
  await mkdir(join(calendar, 'metadata'), { recursive: true });
  await mkdir(join(calendar, 'documents'));
  await writeFile(join(calendar, 'archive.json'), JSON.stringify({ complete: true }));
  for (const [number, year, name, content] of [
    [1, 2026, 'Nutzung von Rechnern durch Lehrkräfte an Potsdamer Schulen', 'Sicherheitsupdates fuer digitale Geraete im Unterricht.'],
    [2, 2026, 'Neubau von bezahlbarem Wohnraum', 'Neue Wohnungen sollen kostenguenstig angeboten werden.'],
    [3, 2026, 'Sanierung von Strassen', 'Der Asphalt wird erneuert und die Fahrbahn ausgebessert.'],
    [4, 2022, 'Haushalt und Finanzen', 'Unnummeriertes altes Formular.'],
  ]) {
    const reference = `${year % 100}/SVV/${String(number).padStart(4, '0')}`;
    const file = { id: id(`files/${number}`), type: schema + 'File', name: `${number}.pdf`,
      fileName: `${number}.pdf`, accessUrl: `https://egov.potsdam.de/public/doc?DOLFDNR=${number}`,
      modified: `${year}-01-01` };
    const document = pdf([null, '<< /Type /Catalog /Pages 2 0 R >>',
      '<< /Type /Pages /Count 2 /Kids [3 0 R 6 0 R] >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
      '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
      stream(`BT /F1 12 Tf 20 760 Td (${reference}) Tj 0 -20 Td (${year === 2026 ? 'Seite: 1/1' : 'Altes Formular'}) Tj 0 -20 Td (${content}) Tj ET`),
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 7 0 R >>',
      stream('BT /F1 12 Tf 20 760 Td (AnlageGeheimnis steht ausschliesslich in der Anlage.) Tj ET')]);
    await writeFile(join(archives, String(year), 'documents', `${hash(file.id)}-${number}.pdf`), document);
    const paper = { id: id(`papers/${number}`), type: schema + 'Paper', name, reference,
      paperType: number === 1 ? 'Kleine Anfrage' : 'Antrag', date: `${year}-03-01`, mainFile: file };
    await writeFile(join(archives, String(year), 'metadata', `${hash(paper.id)}.json`), JSON.stringify(paper));
  }
  run(preparer, ['--input', archives, '--output', prepared]);
  assert.match(run(indexer, buildArgs).stderr, /embeddings omit archive without reference year/);
  const status = JSON.parse(run(searcher, ['--index', index, '--status',
    '--embeddings', embeddings, '--model', model]).stdout);
  assert.equal(status.semantic.titles, 4);
  assert.equal(status.semantic.includedMainForms, 3);
  assert.equal(status.semantic.excludedMainForms, 1, 'zero-content year is explicitly accounted for');
  const natural = 'Computersicherheit für Lehrerinnen und Lehrer';
  assert.equal(query(natural, 'lexical').total, 0);
  assert.equal(query(natural).hits[0].groupIdentity, id('papers/1'));
  assert.equal(query('26/SVV/0001', 'hybrid').hits[0].groupIdentity, id('papers/1'));
  const all = query(natural);
  for (let offset = 0; offset < all.total; offset++) {
    const page = query(natural, 'semantic', ['--limit', '1', '--offset', String(offset),
      '--snapshot-id', all.snapshotId]);
    assert.deepEqual(page.hits, all.hits.slice(offset, offset + 1));
  }
  assert.equal(query(natural, 'semantic', ['--year', '2022']).total, 1);
  assert.equal(query(natural, 'semantic', ['--paper-type', 'Kleine Anfrage']).total, 1);
  assert.equal(query(natural, 'semantic', ['--date-role', 'paper.date', '--date-through', '2022-12-31']).total, 1);
  const pages = query(natural, 'semantic', ['--group', 'record', '--kind', 'Page']);
  assert.ok(pages.hits.length);
  for (const hit of pages.hits) {
    assert.equal(hit.page, 1, 'appendices are not semantic candidates');
    assert.equal(hit.semantic.pageLast, 1);
    assert.equal(hit.semantic.pdfSha256, hit.evidence.citation.documentVersion.sha256);
    assert.ok(hit.semanticText);
    assert.ok(hit.semantic.cosine >= -1 && hit.semantic.cosine <= 1);
    assert.notEqual(hit.preparation.selection, 'matching-text',
      'raw chunk offsets are not asserted as normalized word-match positions');
  }
  const firstManifest = await readFile(join(embeddings, 'manifest.json'));
  const firstIndexHash = hash(await readFile(index));
  const repeated = run(indexer, [...buildArgs, '--embeddings-only']);
  const titleRuns = repeated.stdout.trim().split('\n').map(line => JSON.parse(line));
  assert.equal(titleRuns.length, 2);
  assert.ok(titleRuns.every(item => item.computed === 0 && item.model_loads === 0));
  assert.equal(titleRuns.reduce((total, item) => total + item.reused, 0), 4);
  const progress = repeated.stderr.split('\n').filter(line => line.startsWith('{')).map(line => JSON.parse(line));
  const completed = progress.filter(item => item.phase === 'complete');
  assert.equal(completed.length, 2);
  assert.ok(completed.every(item => item.computed_vectors === 0 && item.model_loads === 0));
  assert.equal(completed.reduce((total, item) => total + item.reused_chunks, 0), 3);
  assert.equal(hash(await readFile(index)), firstIndexHash);
  const descriptor = JSON.parse(await readFile(join(embeddings, 'manifest.json'), 'utf8'));
  assert.notEqual(descriptor.generation, JSON.parse(firstManifest).generation);
  const reservation = createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  child = spawn(searcher, ['--index', index, '--embeddings', embeddings, '--model', model,
    '--threads', '2', '--serve', '--port', String(port)], { stdio: ['ignore', 'ignore', 'pipe'] });
  let log = '';
  child.stderr.on('data', bytes => { log += bytes; });
  const get = path => fetch(`http://127.0.0.1:${port}${path}`, { signal: AbortSignal.timeout(15000) });
  const deadline = Date.now() + 15000;
  for (;;) {
    assert.equal(child.exitCode, null, log);
    try { assert.equal((await get('/api/semantic-status')).status, 200); break; }
    catch (error) { if (Date.now() >= deadline) throw error; await delay(25); }
  }
  const response = await get('/api/search?' + new URLSearchParams({ q: natural, mode: 'semantic', group: 'paper' }));
  assert.equal(response.status, 200);
  assert.deepEqual((await response.json()).hits, all.hits);
  const mcp = await fetch(`http://127.0.0.1:${port}/mcp`, {
    method: 'POST', signal: AbortSignal.timeout(15000), headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/call',
      params: { name: 'search', arguments: { q: natural, mode: 'semantic', group: 'paper' } } }),
  });
  const reply = await mcp.json();
  assert.equal(reply.result.isError, false);
  assert.deepEqual(reply.result.structuredContent.hits, all.hits);
  assert.match(await (await get('/')).text(), /name="mode"/);
  child.kill('SIGTERM'); await once(child, 'exit'); child = undefined;
  const manifestPath = join(embeddings, 'manifest.json');
  const saved = await readFile(manifestPath);
  await writeFile(manifestPath, JSON.stringify({ ...descriptor, snapshotId: '0'.repeat(64) }));
  assert.match(run(searcher, ['--index', index, '--embeddings', embeddings, '--model', model, natural], 1).stderr,
    /mismatched OParl snapshot/);
  await writeFile(manifestPath, saved);
  await writeFile(join(embeddings, '.oparl-embedding.lock'), 'test owner');
  assert.match(run(indexer, [...buildArgs, '--embeddings-only'], 1).stderr, /locked/);
  assert.deepEqual(await readFile(manifestPath), saved);
  await rm(join(embeddings, '.oparl-embedding.lock'));
  const cache = join(embeddings, 'generations', descriptor.generation, '2026', 'titles', 'cache');
  const vector = join(cache, (await readdir(cache)).find(name => name.endsWith('.egv')));
  const original = await readFile(vector), bytes = Buffer.from(original);
  bytes[100] ^= 1; await writeFile(vector, bytes);
  assert.match(run(searcher, ['--index', index, '--embeddings', embeddings, '--model', model, natural], 1).stderr,
    /cached vector failed verification/);
  await writeFile(vector, original);
  const changedPdf = join(archives, '2026', 'documents', `${hash(id('files/1'))}-1.pdf`);
  await writeFile(changedPdf, Buffer.concat([await readFile(changedPdf), Buffer.from('\n% changed PDF version\n')]));
  run(preparer, ['--input', archives, '--output', prepared]);
  const changedIndex = join(root, 'changed.ops');
  run(indexer, ['--input', archives, '--prepared', prepared, '--output', changedIndex]);
  const changedStatus = JSON.parse(run(searcher, ['--index', changedIndex, '--status']).stdout);
  await writeFile(manifestPath, JSON.stringify({ ...descriptor, snapshotId: changedStatus.snapshotId }));
  assert.match(run(searcher, ['--index', changedIndex, '--embeddings', embeddings, '--model', model, natural], 1).stderr,
    /embedded PDF version does not match/);
  console.log('Native embedding pipeline, semantic/hybrid filtering, provenance, cache and API tests passed');
} finally {
  if (child && child.exitCode === null) { child.kill('SIGTERM'); await once(child, 'exit'); }
  await rm(root, { recursive: true, force: true });
}

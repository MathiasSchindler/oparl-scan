import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, realpath, rename, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createServer } from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { pdf, stream } from './pdf-fixture.mjs';

const [prepare, indexer, searcher] = process.argv.slice(2).map(p => resolve(p));
assert.ok(prepare && indexer && searcher, 'Provide prepare, indexer and searcher paths');
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-prepare-')));
const archive = join(root, 'archive'), cache = join(root, 'prepared');
const hash = data => createHash('sha256').update(data).digest('hex');
const id = path => `https://fixture.invalid/oparl/${path}`;
const schema = 'https://schema.oparl.org/1.1/';
const index = join(root, 'prepared.ops');
function run(binary, args, expected = 0) {
  const result = spawnSync(binary, args, { encoding: 'utf8', timeout: 60000, maxBuffer: 8 * 1024 ** 2 });
  assert.equal(result.status, expected, `${result.error ?? ''}\n${result.stderr}\n${result.stdout}`);
  return result;
}
const prepareRun = (input = archive, extra = [], expected = 0) =>
  JSON.parse(run(prepare, ['--input', input, '--output', cache, ...extra], expected).stdout);
const show = (object, page, which = index) => JSON.parse(run(searcher, [
  '--index', which, '--json', '--raw', '--show', object, ...(page ? ['--page', String(page)] : []),
]).stdout);
function textPdf(lines, secondPage = false) {
  const objects = [null, '<< /Type /Catalog /Pages 2 0 R >>',
    `<< /Type /Pages /Kids [3 0 R${secondPage ? ' 6 0 R' : ''}] /Count ${secondPage ? 2 : 1} >>`,
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
    stream(`BT /F1 12 Tf 16 TL 1 0 0 1 40 740 Tm ${lines.map((line, i) => {
      const [position, s] = Array.isArray(line)
        ? [`1 0 0 1 ${line[0]} ${line[1]} Tm `, line[2]] : [i ? 'T* ' : '', line];
      return `${position}(${s.replaceAll('\\', '\\\\').replaceAll('(', '\\(').replaceAll(')', '\\)')}) Tj`;
    }).join('\n')} ET`)];
  if (secondPage) objects.push(
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 4 0 R >> >> /Contents 7 0 R >>',
    stream('BT /F1 12 Tf 16 TL 1 0 0 1 40 740 Tm (Fernwaer-) Tj T* (me) Tj ET'));
  return pdf(objects);
}
const header = (date = '16.07.2026', extra = '') => [
  `26/SVV/0677-002 Kleine Anfrage oeffentlich ${extra}`,
  `Einreicher: GB 5 Zentrale Verwaltung Datum ${date}`,
  'Anlass des Auskunftsersuchens:',
  '1. Bestaetigt die Verwaltung die Berechtigung?',
  'Ja. Diese Berechtigung ergibt sich aus der Satzung.',
];
async function artifact(bytes, directory = cache) {
  const manifest = JSON.parse(await readFile(join(directory, 'manifest.json'), 'utf8'));
  const path = join(directory, manifest.key, `${hash(bytes)}.json`);
  const data = JSON.parse(await readFile(path, 'utf8'));
  assert.equal(data.sha256, hash(JSON.stringify(data.data)), 'Envelope authenticates the exact serialized payload');
  assert.equal(data.data.sourceSha256, hash(bytes));
  assert.equal(data.data.key, manifest.key);
  return { path, data: data.data };
}
async function preparedApi(file, fields) {
  if (process.platform === 'win32') return;
  const reservation = createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const child = spawn(searcher, ['--index', index, '--serve', '--port', String(port)],
    { stdio: ['ignore', 'ignore', 'pipe'] });
  let log = '', launchError;
  child.stderr.on('data', data => { log += data; });
  child.on('error', error => { launchError = error; });
  const ended = once(child, 'exit');
  const base = `http://127.0.0.1:${port}`;
  const get = path => fetch(base + path, { signal: AbortSignal.timeout(2000) });
  try {
    const deadline = Date.now() + 8000;
    for (;;) {
      if (launchError) throw launchError;
      assert.equal(child.signalCode, null, log);
      assert.equal(child.exitCode, null, log);
      const response = await get('/api/status').catch(error => {
        if (error.cause?.code === 'ECONNREFUSED') return null;
        throw error;
      });
      if (response?.ok) {
        assert.equal((await response.json()).formatVersion, 4);
        break;
      }
      assert.ok(Date.now() < deadline, `Prepared server did not become ready: ${log}`);
      await delay(25);
    }
    const object = await (await get('/api/object?' + new URLSearchParams({ id: file }))).json();
    assert.deepEqual(object.preparation.fields, fields);
    const response = await fetch(base + '/mcp', {
      method: 'POST', signal: AbortSignal.timeout(3000),
      headers: { 'Content-Type': 'application/json', 'MCP-Protocol-Version': '2025-11-25' },
      body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/call',
        params: { name: 'get_object', arguments: { id: file } } }),
    });
    const body = await response.text(), reply = JSON.parse(body);
    assert.equal(response.status, 200);
    assert.ok(Buffer.byteLength(body) < 20000, 'Prepared default MCP replies stay below 20,000 actual body bytes');
    assert.deepEqual(reply.result.structuredContent.preparation.fields, fields);
  } finally {
    child.kill('SIGTERM');
    const timeout = setTimeout(() => child.kill('SIGKILL'), 2000);
    try { await ended; } finally { clearTimeout(timeout); }
  }
}
try {
  await mkdir(join(archive, 'metadata'), { recursive: true });
  await mkdir(join(archive, 'documents'));
  await writeFile(join(archive, 'archive.json'), JSON.stringify({ referenceYear: 2026, complete: true, metadataOnly: false }));
  const source = textPdf(header(), true), file = id('files/answer'), paper = id('papers/question');
  const sourcePath = join(archive, 'documents', `${hash(file)}-answer.sao`);
  await writeFile(sourcePath, source);
  await writeFile(join(archive, 'documents', 'incomplete.pdf.part'), 'not a complete PDF');
  for (const object of [
    { id: file, type: schema + 'File', name: 'Answer', created: '2026-07-20T12:00:00Z' },
    { id: paper, type: schema + 'Paper', name: 'Test question', reference: '26/SVV/0677-002', paperType: 'Kleine Anfrage', mainFile: file },
  ]) await writeFile(join(archive, 'metadata', `${hash(object.id)}.json`), JSON.stringify(object));

  assert.equal(prepareRun().documents, 1, 'Archive documents need no .pdf extension; partial downloads are excluded');
  const initial = await artifact(source);
  assert.equal(initial.data.pageCount, 2);
  assert.equal(initial.data.pages[1].text, 'Fernwaerme');
  assert.ok(!initial.data.pages[1].layout, 'Default artifacts keep only first-page positions');
  const date = initial.data.fields.find(f => f.field === 'document.date');
  assert.equal(date.value, '2026-07-16');
  assert.equal(date.sourceText, '16.07.2026');
  assert.equal(date.status, 'rule-supported');
  assert.equal(initial.data.fields.find(f => f.field === 'originator.label').value, 'GB 5 Zentrale Verwaltung');
  assert.ok(!initial.data.fields.some(f => f.field === 'document.role'), 'Raw type/suffix must not invent an answer role');
  const rawPage = Buffer.from(initial.data.pages[0].layout.text);
  assert.equal(rawPage.subarray(date.layoutTextStart, date.layoutTextStart + date.layoutTextLength).toString(), date.sourceText);
  for (const span of initial.data.pages[0].layout.spans) {
    assert.ok(span.start + span.length <= rawPage.length);
    assert.ok(span.x >= 40000 && span.y <= 740000);
  }
  const spacedLines = ['Erste Zeile ', 'Zweite Zeile ', 'Dritte Zeile'];
  const spaced = textPdf(spacedLines), spacedPath = join(root, 'trailing-spaces.pdf');
  await writeFile(spacedPath, spaced);
  prepareRun(spacedPath);
  const spacedLayout = (await artifact(spaced)).data.pages[0].layout;
  assert.equal(spacedLayout.text, spacedLines.join('\n'), 'Trailing PDF spaces must not suppress line breaks');
  for (const [i, span] of spacedLayout.spans.entries()) {
    assert.equal(Buffer.from(spacedLayout.text).subarray(span.start, span.start + span.length).toString(),
      spacedLines[i], 'Adding line breaks must preserve every original span offset');
  }
  const stamp = (await stat(initial.path, { bigint: true })).mtimeNs;
  run(prepare, ['--input', archive, '--output', join(archive, 'derived')], 1);
  assert.equal(prepareRun().reused, 1);
  assert.equal((await stat(initial.path, { bigint: true })).mtimeNs, stamp, 'Cache hits must not rewrite artifacts');
  assert.deepEqual(await readFile(sourcePath), source, 'Original bytes remain unchanged');

  const directIndex = join(root, 'direct.ops');
  run(indexer, ['--input', archive, '--output', directIndex]);
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache]);
  assert.equal(show(file, 1).text, show(file, 1, directIndex).text);
  assert.equal(show(file, 2).text, show(file, 2, directIndex).text);
  assert.deepEqual(show(file).metadata, show(file, undefined, directIndex).metadata, 'Original metadata is not rewritten');
  assert.equal(show(paper).metadata.paperType, 'Kleine Anfrage');
  assert.deepEqual(show(file).preparation.fields, initial.data.fields);
  const modelDate = show(file).model.dates['document.date'];
  assert.equal(modelDate.value, '2026-07-16');
  assert.equal(modelDate.evidence.page, 1);
  assert.equal(modelDate.evidence.certainty, 'derived');
  assert.equal(modelDate.evidence.documentVersion, show(file).model.documentVersion.id);
  const filtered = JSON.parse(run(searcher, ['--index', index, '--json', '--group', 'document',
    '--date-role', 'document.date', '--date-from', '2026-07-16', '--date-through', '2026-07-16', 'Berechtigung']).stdout);
  assert.equal(filtered.total, 1, 'Prepared document date is available to normal date filters');
  assert.equal(JSON.parse(run(searcher, ['--index', index, '--status']).stdout).formatVersion, 4);
  assert.equal(JSON.parse(run(searcher, ['--index', directIndex, '--status']).stdout).formatVersion, 3);
  await rename(archive, join(root, 'hidden-archive'));
  await rename(cache, join(root, 'hidden-cache'));
  assert.equal(show(file, 2).text, 'Fernwaerme', 'Search remains self-contained without archive/preparation access');
  assert.deepEqual(show(file).preparation.fields, initial.data.fields);
  await preparedApi(file, initial.data.fields);
  await rename(join(root, 'hidden-archive'), archive);
  await rename(join(root, 'hidden-cache'), cache);

  const changed = textPdf(header('17.07.2026'), true);
  await writeFile(sourcePath, changed);
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache], 1);
  assert.equal(show(file).preparation.sourceSha256, hash(source), 'Missing preparation never replaces an existing index');
  prepareRun();
  assert.equal((await artifact(changed)).data.fields[0].value, '2026-07-17');
  await writeFile(sourcePath, source);

  const corrupt = JSON.parse(await readFile(initial.path, 'utf8'));
  corrupt.data.pages[0].text += ' altered';
  await writeFile(initial.path, JSON.stringify(corrupt));
  run(prepare, ['--input', archive, '--output', cache], 1);
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache], 1);
  assert.equal(prepareRun(archive, ['--force']).reused, 0);
  const cachedText = JSON.parse(await readFile(initial.path, 'utf8'));
  cachedText.data.pages[1].text = 'prepared-cache-sentinel';
  cachedText.sha256 = hash(JSON.stringify(cachedText.data));
  await writeFile(initial.path, JSON.stringify(cachedText));
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache]);
  assert.equal(show(file, 2).text, 'prepared-cache-sentinel', 'Prepared imports consume validated cache text, not newly parsed PDF text');
  prepareRun(archive, ['--force']);
  const malformed = JSON.parse(await readFile(initial.path, 'utf8'));
  malformed.data.fields = null;
  malformed.sha256 = hash(JSON.stringify(malformed.data));
  await writeFile(initial.path, JSON.stringify(malformed));
  run(prepare, ['--input', archive, '--output', cache], 1);
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache], 1);
  prepareRun(archive, ['--force']);
  const manifestPath = join(cache, 'manifest.json'), manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
  await writeFile(manifestPath, JSON.stringify({ ...manifest, engine: 'stale-engine' }));
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache], 1);
  prepareRun();

  const expanded = prepareRun(archive, ['--layout']);
  assert.notEqual(expanded.key, initial.data.key);
  const expandedDoc = await artifact(source);
  assert.match(expandedDoc.data.pages[1].layout.text, /Fernwaer-\nme/);
  const customPath = join(root, 'templates.json');
  const custom = JSON.parse(await readFile('templates/potsdam.json', 'utf8'));
  await writeFile(customPath, JSON.stringify(custom));
  const customRun = prepareRun(archive, ['--templates', customPath]);
  assert.notEqual(customRun.key, initial.data.key, 'Template bytes participate in invalidation');
  assert.equal(prepareRun(archive, ['--templates', customPath]).reused, 1);
  custom.templates.push({ ...custom.templates[0], id: 'equally-specific' });
  await writeFile(customPath, JSON.stringify(custom));
  prepareRun(archive, ['--templates', customPath]);
  assert.equal((await artifact(source)).data.ambiguousHeaders, 1);
  assert.deepEqual((await artifact(source)).data.fields, [], 'Tied layouts produce no invented field values');
  custom.templates[0].typo = true;
  await writeFile(customPath, JSON.stringify(custom));
  run(prepare, ['--input', archive, '--output', cache, '--templates', customPath], 1);

  for (const [name, lines, expected] of [
    ['unknown', ['Not a recognized Potsdam form. Datum 16.07.2026'], { unmatchedHeaders: 1 }],
    ['invalid-date', header('31.02.2026'), { fieldStatus: 'invalid' }],
    ['placeholder', header('01.01.2000'), { fieldStatus: 'placeholder' }],
    ['duplicate-date', [header()[0], 'Einreicher: GB 5 Datum 16.07.2026 Datum 17.07.2026'], { fieldStatus: 'ambiguous' }],
    ['extension', header('16.07.2026', 'Fristverlängerungsbitte'), { role: 'deadline-extension' }],
    ['answer', ['26/SVV/0123 Antwort auf Kleine Anfrage', 'Geschäftsbereich: GB 5 Datum 16.07.2026',
      'Antwort der Verwaltung:', 'Inhalt'], { role: 'answer', department: 'GB 5' }],
    ['answer-extension', ['26/SVV/0123 Antwort auf Kleine Anfrage - Fristverlängerungsbitte',
      'Geschäftsbereich: GB 5 Datum 16.07.2026', 'Antwort der Verwaltung:', 'Wir bitten um mehr Zeit.'],
    { role: 'deadline-extension' }],
    ['mentioned-extension', [...header(), 'Eine Fristverlängerungsbitte wird nur in der Antwort erwaehnt.'], { noRole: true }],
  ]) {
    const bytes = textPdf(lines), path = join(root, `${name}.pdf`);
    await writeFile(path, bytes);
    prepareRun(path);
    const found = (await artifact(bytes)).data;
    if (expected.unmatchedHeaders) assert.equal(found.unmatchedHeaders, 1);
    if (expected.fieldStatus) {
      assert.equal(found.fields[0].status, expected.fieldStatus);
      assert.equal(found.fields[0].value, null);
    }
    if (expected.role) assert.equal(found.fields.find(f => f.field === 'document.role').value, expected.role);
    if (expected.noRole) assert.ok(!found.fields.some(f => f.field === 'document.role'), 'Body mentions do not classify a document');
    if (expected.department) {
      assert.equal(found.fields.find(f => f.field === 'department.label').value, expected.department);
      assert.ok(!found.fields.some(f => f.field === 'originator.label'), 'A department heading does not prove submission/authorship');
    }
  }
  const matrix = pdf([null, '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /XObject << /F 4 0 R >> >> /Contents 5 0 R >>',
    Buffer.concat([Buffer.from('<< /Type /XObject /Subtype /Form /BBox [0 0 600 800] /Matrix [1 0 0 1 30 10] /Resources << /Font << /F1 6 0 R >> >> '),
      stream('BT /F1 12 Tf 1 0 0 1 20 700 Tm (Matrix test) Tj ET').subarray(3)]),
    stream('/F Do'), '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>']);
  const matrixPath = join(root, 'matrix.pdf');
  await writeFile(matrixPath, matrix);
  prepareRun(matrixPath);
  assert.equal((await artifact(matrix)).data.pages[0].layout.spans[0].x, 50000);
  assert.equal((await artifact(matrix)).data.pages[0].layout.spans[0].y, 710000);
  for (const [gap, expected] of [[4, 'rule-supported'], [14, 'rule-supported'], [22, 'rule-supported'],
    [23, 'unsupported-placement'], [80, 'unsupported-placement']]) {
    const below = pdf([null, '<< /Type /Catalog /Pages 2 0 R >>',
      '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
      '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
      '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
      stream(`BT /F1 12 Tf 1 0 0 1 40 740 Tm (26/SVV/0677-002) Tj
        1 0 0 1 40 700 Tm (Einreicher:) Tj 1 0 0 1 40 ${700-gap} Tm (GB 5) Tj
        1 0 0 1 440 700 Tm (Datum) Tj 1 0 0 1 440 ${700-gap} Tm (16.07.2026) Tj ET`)]);
    const path = join(root, `below-${gap}.pdf`);
    await writeFile(path, below);
    prepareRun(path);
    assert.equal((await artifact(below)).data.fields[0].status, expected, 'Below-label fields require bounded geometric proximity');
  }
  const legacyQuestion = [
    [40, 780, 'Kleine Anfrage Drucksache Nr. (ggf. Nachtragsvermerk)'],
    [400, 750, '23/SVV/0576'],
    [40, 720, 'Einreicher: Fraktion Beispiel'],
    [400, 720, 'öffentlich'],
    [40, 680, 'Betreff: Testfrage'],
    [244, 638, 'Erstellungsdatum:'], [400, 636.6, '06.06.2023'],
    [244, 620, 'Eingang Büro der SVV:'], [400, 618.6, '09.06.2023'],
    [244, 602, 'weitergeleitet an das Büro OBM:'], [400, 600.6, '12.06.2023'],
    [244, 584, 'Termin der Beantwortung:'], [400, 582.6, '30.06.2023'],
    [244, 566, 'Terminverlängerung:'],
    [244, 548, 'Eingang der Beantwortung:'], [400, 546.6, '05.07.2023'],
    [40, 490, 'Anlass des Auskunftsersuchens:'],
    [40, 474, 'Erstellungsdatum: 01.01.1900 ist hier nur ein Zitat.'],
  ];
  for (const [name, lines, expected] of [
    ['legacy-question', legacyQuestion, {
      'document.date': '2023-06-06', 'submission.receivedDate': '2023-06-09',
      'submission.forwardedDate': '2023-06-12', 'answer.dueDate': '2023-06-30',
      'answer.receivedDate': '2023-07-05', 'originator.label': 'Fraktion Beispiel',
    }],
    ['legacy-paper', [
      [40, 780, 'Antrag Drucksache Nr. (ggf. Nachtragsvermerk)'], [400, 750, '23/SVV/0359'],
      [395, 638, 'Erstellungsdatum'], [496, 636.6, '06.04.2023'],
      [395, 620, 'Freigabedatum:'], [496, 618.6, '07.04.2023'],
      [40, 636.6, 'Einreicher: Fraktion Beispiel'],
      [40, 570, 'Beratungsfolge:'], [40, 550, 'Datum der Sitzung Gremium Zuständigkeit'],
      [40, 530, '03.05.2023 Stadtverordnetenversammlung Entscheidung'],
    ], { 'document.date': '2023-04-06', 'document.releaseDate': '2023-04-07',
      'originator.label': 'Fraktion Beispiel' }],
    ['legacy-short-question', [
      [40, 780, 'Anfrage Drucksache Nr. (ggf. Nachtragsvermerk)'], [400, 750, '23/SVV/0436'],
      [40, 710, 'Einreicher:'], [40, 696, 'Fraktion Beispiel'],
      [40, 670, 'Betreff:'], [40, 654, 'Testfrage'],
      [392, 614, 'Erstellungsdatum:'], [496, 612.6, '27.04.2023'],
      [392, 595, 'Freigabedatum:'],
      [40, 570, 'Datum der Sitzung:'], [40, 554, '03.05.2023'],
      [40, 510, 'Anlass des Auskunftsersuchens:'], [40, 494, 'Inhalt der Frage.'],
    ], { 'document.date': '2023-04-27', 'originator.label': 'Fraktion Beispiel' }],
  ]) {
    const bytes = textPdf(lines), path = join(root, `${name}.pdf`);
    await writeFile(path, bytes);
    prepareRun(path);
    const found = (await artifact(bytes)).data;
    assert.equal(found.matchedHeaders, 1, 'Legacy forms must not tie with generic date headings');
    assert.ok(!found.fields.some(f => f.field === 'document.role'), 'Receipt and due dates do not prove an answer role');
    for (const [field, value] of Object.entries(expected)) {
      const extracted = found.fields.find(f => f.field === field);
      assert.equal(extracted.status, 'rule-supported', `${name}: ${field}`);
      assert.equal(extracted.value, value, `${name}: ${field}`);
      const raw = Buffer.from(found.pages[0].layout.text);
      assert.equal(raw.subarray(extracted.layoutTextStart, extracted.layoutTextStart + extracted.layoutTextLength).toString(),
        extracted.sourceText, 'Date roles and actors retain byte-exact layout evidence');
    }
    if (name === 'legacy-short-question') {
      assert.equal(found.fields.find(f => f.field === 'document.releaseDate').status, 'missing',
        'An empty release cell cannot borrow a meeting heading or meeting date');
    }
    if (name === 'legacy-question') {
      const empty = found.fields.find(f => f.field === 'answer.extendedDueDate');
      assert.equal(empty.status, 'missing', 'A blank deadline extension cannot borrow the following answer receipt date');
      assert.equal(empty.value, null);
      assert.equal(empty.sourceText, '');
      await writeFile(sourcePath, bytes);
      prepareRun();
      run(indexer, ['--input', archive, '--output', index, '--prepared', cache]);
      assert.equal(show(file).model.dates['document.date'].value, '2023-06-06');
      assert.deepEqual(show(file).preparation.fields, found.fields, 'New date roles survive standalone index import');
      assert.ok(!show(file).model.dates.submittedAt, 'A receipt date must not become a submission creation date');
      await preparedApi(file, found.fields);
      await writeFile(sourcePath, source);
    }
  }
  for (const [name, x, gap, count, status] of [
    ['aligned', 40, 14, 2, 'rule-supported'],
    ['minimum-gap', 40, 4, 2, 'rule-supported'],
    ['maximum-gap', 40, 22, 2, 'rule-supported'],
    ['alignment-edge', 44, 14, 2, 'rule-supported'],
    ['four-lines', 40, 14, 4, 'rule-supported'],
    ['wrong-column', 45, 14, 2, 'unsupported-placement'],
    ['too-distant', 40, 23, 2, 'unsupported-placement'],
    ['too-many-lines', 40, 14, 5, 'unsupported-placement'],
  ]) {
    const lines = [[40, 740, '26/SVV/0677'], [40, 700, 'Einreicher:'],
      ...Array.from({ length: count }, (_, i) => [i ? x : 40, 686 - i * gap, `Fraktion ${i + 1} `]),
      [440, 700, 'Datum'], [440, 686, '16.07.2026'], [40, 560, 'Anlass des Auskunftsersuchens:']];
    const bytes = textPdf(lines), path = join(root, `multiline-${name}.pdf`);
    await writeFile(path, bytes);
    prepareRun(path);
    const found = (await artifact(bytes)).data;
    const actor = found.fields.find(f => f.field === 'originator.label');
    assert.equal(actor.status, status, name);
    if (status === 'rule-supported') {
      assert.equal(actor.value, Array.from({ length: count }, (_, i) => `Fraktion ${i + 1}`).join(' '));
      assert.match(actor.sourceText, /\n/, 'Normalized actors retain original multiline evidence');
    } else assert.equal(actor.value, null, 'Reject the complete field rather than returning a truncated actor');
  }
  const invalidConfig = JSON.parse(await readFile('templates/potsdam.json', 'utf8'));
  invalidConfig.templates[0].fields[1].maxLines = 5;
  await writeFile(customPath, JSON.stringify(invalidConfig));
  run(prepare, ['--input', sourcePath, '--output', cache, '--templates', customPath], 1);
  const image = pdf([null, '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Resources << /XObject << /I 4 0 R >> >> /Contents 5 0 R >>',
    Buffer.from('<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8 /Length 1 >>\nstream\nx\nendstream'),
    stream('/I Do')]);
  const imagePath = join(root, 'scan.pdf');
  await writeFile(imagePath, image);
  prepareRun(imagePath, [], 3);
  assert.equal((await artifact(image)).data.pages[0].warnings & 4, 4, 'Image-only PDFs retain explicit OCR gaps');
  const badPath = join(root, 'broken.pdf');
  await writeFile(badPath, 'not a PDF');
  assert.equal(prepareRun(badPath, [], 3).warningOrFailedDocuments, 1);
  assert.equal((await artifact(Buffer.from('not a PDF'))).data.status, 'failed');

  const secondFile = id('files/independent'), secondPaper = id('papers/independent');
  await writeFile(join(archive, 'documents', `${hash(secondFile)}-same-bytes.sao`), source);
  for (const object of [
    { id: secondFile, type: schema + 'File', name: 'Same bytes, different resource' },
    { id: secondPaper, type: schema + 'Paper', name: 'Independent paper', reference: '26/SVV/0999', mainFile: secondFile },
  ]) await writeFile(join(archive, 'metadata', `${hash(object.id)}.json`), JSON.stringify(object));
  prepareRun();
  run(indexer, ['--input', archive, '--output', index, '--prepared', cache]);
  assert.notEqual(show(file).model.documentVersion.id, show(secondFile).model.documentVersion.id);
  assert.equal(show(file).model.documentVersion.sha256, show(secondFile).model.documentVersion.sha256);
  const papers = JSON.parse(run(searcher, ['--index', index, '--json', '--group', 'paper', 'Berechtigung']).stdout);
  assert.deepEqual(papers.hits.map(h => h.relatedPaper).sort(), [paper, secondPaper].sort(),
    'Byte-sharing must preserve independent resource identities and direct Paper ownership');
  // Repeated bytes share extraction, but traversal must not stop at 1,024 entries.
  await Promise.all(Array.from({ length: 1030 }, (_, i) =>
    writeFile(join(archive, 'documents', `${String(i).padStart(64, '0')}-copy.sao`), source)));
  const many = prepareRun();
  assert.equal(many.documents, 1032);
  assert.equal(many.reused, 1032);
  console.log('Prepare tests passed: text/layout parity, conservative header fields, cache reuse/invalidation/corruption, standalone prepared index, provenance, source preservation, form transforms and >1024 files.');
} finally {
  await rm(root, { recursive: true, force: true });
}

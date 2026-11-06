import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdtemp, mkdir, readFile, realpath, rm, utimes, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pdf, stream } from './pdf-fixture.mjs';
import { testSearchWeb } from './search-web.mjs';

const [indexer, searcher] = process.argv.slice(2);
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-search-test-')));
const schema = 'https://schema.oparl.org/1.1/';
const origin = 'https://www.potsdam.sitzung-online.de/oparl/';
const hash = text => createHash('sha256').update(text).digest('hex');
const id = path => `${origin}${path}`;
const escapedName = 'Person 1099 "Zitat" \\ Pfad\tZeile\n\b\f\u0001';
function run(program, args, expected = 0, input) {
  const result = spawnSync(program, args, { encoding: 'utf8', input, timeout: 30000, maxBuffer: 8 * 1024 * 1024 });
  assert.equal(result.status, expected, `${result.error ?? result.signal ?? ''}\n${result.stderr}\n${result.stdout}`);
  return result;
}
const shared = pdf([null,
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Count 2 /Kids [6 0 R 3 0 R] /Resources << /Font << /F1 4 0 R /F2 11 0 R /F3 12 0 R >> >> >>',
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 8 0 R >>',
  '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
  stream('BT /F1 12 Tf 10 700 Td (Ein sicherer Schulweg verbessert die Sicherheit.) Tj T* (Die Stra\\337e ist lang.) Tj ET\nBT /F1 12 Tf 1 0 0 1 10 600 Tm (Alpha) Tj 1 0 0 1 50 600 Tm (Beta) Tj ET\nBT /F1 12 Tf 1 0 0 1 10 500 Tm (K) Tj 1 0 0 1 16 500 Tm (ind) Tj ET\nBT /F1 12 Tf q /F2 12 Tf (A) Tj Q (A) Tj ET\nBT /F3 10 Tf 1 0 0 1 10 400 Tm (Nove) Tj 1 0 0 1 50 400 Tm (mber ) Tj 1 0 0 1 100 400 Tm (202) Tj 1 0 0 1 130 400 Tm (5) Tj ET\nBT /F1 12 Tf 10 300 Td (Jahresab-) Tj 0 -12 Td (schluss 2024) Tj 0 -12 Td (Druck-) Tj 0 -12 Td (und Kopierkosten) Tj ET\nBT /F3 10 Tf 10 200 Td (A) Tj 10 0 Td (mt) Tj ET\nBT /F3 10 Tf 10 150 Td (Ver) Tj ET BT /F3 10 Tf 40 150 Td (waltung) Tj ET\nq 1 0 0 1 0 100 cm BT /F3 10 Tf 10 10 Td (Nord) Tj ET Q q 1 0 0 1 0 90 cm BT /F3 10 Tf 10 10 Td (Sued) Tj ET Q'),
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 7 0 R >>',
  stream('BT /F1 12 Tf 10 700 Td [(Die erste) -250 (Seite beschreibt Radverkehr.)] TJ ET'),
  '[9 0 R 5 0 R 10 0 R]',
  stream('q'),
  stream('Q'),
  '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding << /BaseEncoding /WinAnsiEncoding /Differences [65 /B] >> >>',
  '<< /Type /Font /Subtype /TrueType /BaseFont /Test /Encoding /WinAnsiEncoding /FirstChar 32 /LastChar 122 /Widths 13 0 R >>',
  '[' + Array(91).fill('1000').join(' ') + ']',
]);
const unicode = pdf([null,
  '<< /Type /Catalog /Pages 2 0 R >>',
  '<< /Type /Pages /Count 1 /Kids [3 0 R] >>',
  '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
  '<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H /ToUnicode 6 0 R /DescendantFonts [7 0 R] >>',
  stream('BT /F1 12 Tf 10 700 Td <0001000200030004000500200006000700080009> Tj ET'),
  stream('/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n/CMapName /Test def /CMapType 2 def\n1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n6 beginbfchar\n<0001> <0047>\n<0002> <0072>\n<0003> <00F6>\n<0020> <0020>\n<0008> <00660069>\n<0009> <D83DDE00>\nendbfchar\n2 beginbfrange\n<0004> <0005> [<00DF> <0065>]\n<0006> <0007> <0041>\nendbfrange\nendcmap end end'),
  '<< /Type /Font /Subtype /CIDFontType2 /BaseFont /Test /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> >>',
]);
const file = number => ({ id: id(`files/${number}`), type: `${schema}File`, name: `${number}.pdf`, fileName: `${number}.pdf`,
  accessUrl: `https://egov.potsdam.de/public/doc?DOCTYP=130&DOLFDNR=${number}&OTYP=41`,
  ...(number === 1 ? { downloadUrl: 'https://egov.potsdam.de/public/doc?DOCTYP=130&DOLFDNR=1&OTYP=41&download=1' } : {}),
  modified: '2026-01-01' });
const archives = join(root, 'archives');
const index = join(root, 'index.ops');
async function metadata(year, object) {
  await writeFile(join(archives, String(year), 'metadata', `${hash(object.id)}.json`), JSON.stringify(object));
}
try {
  for (const year of [2023, 2024]) {
    const directory = join(archives, String(year));
    await mkdir(join(directory, 'metadata'), { recursive: true });
    await mkdir(join(directory, 'documents'));
    await writeFile(join(directory, 'archive.json'), JSON.stringify({ complete: true, referenceYear: year }));
    await writeFile(join(directory, 'documents', `${hash(file(1).id)}-1.pdf`), shared);
  }
  for (const [number, name] of [[1, 'Fraktion Alpha'], [2, 'Fraktion Beta']])
    await metadata(2023, { id: id(`organizations/${number}`), Type: `${schema}Organization`, type: 'at', name });
  for (let number = 0; number < 1100; number++)
    await metadata(2023, { id: id(`people/${number}`), type: `${schema}Person`,
      name: number === 1099 ? escapedName : `Person ${number}`,
      ...(number < 24 ? { date: `${2020 + number % 4}-01-01` } : {}) });
  for (const [number, year, name, paperType, organization, attachment] of [
    [1, 2023, 'Schulweg vertraulich', 'Antrag', 1, 1],
    [2, 2024, 'Haushalt öffentlich', 'Antwort', 2, 1],
    [3, 2024, 'Unicode Dokument', 'Antrag', 1, 2],
  ]) await metadata(year, {
    id: id(`papers/${number}`), type: `${schema}Paper`, reference: `${year % 100}/SVV/${number}`,
    name, date: `${year}-01-01`, paperType, underDirectionOf: [id(`organizations/${organization}`)],
    mainFile: file(attachment), modified: '2026-01-01',
  });
  await metadata(2024, { id: id('papers/1'), type: `${schema}Paper`, reference: '23/SVV/1',
    name: 'Veraltet', modified: '2020-01-01' });
  await metadata(2023, { id: id('papers/deleted'), type: `${schema}Paper`, name: 'Deleted unique', modified: '2020-01-01' });
  await utimes(join(archives, '2023', 'metadata', `${hash(id('papers/deleted'))}.json`), 1000, 1000);
  await metadata(2024, { id: id('papers/deleted'), type: `${schema}Paper`, name: 'Deleted unique', deleted: true });
  await writeFile(join(archives, '2024', 'documents', `${hash(file(2).id)}-2.pdf`), unicode);
  const built = run(indexer, ['--input', archives, '--output', index]);
  assert.match(built.stderr, /pages=3,/);
  const query = (text, args = []) => JSON.parse(run(searcher, ['--index', index, '--json', ...args, text]).stdout);
  const papers = text => query(text, ['--kind', 'Paper']).hits.map(hit => hit.id).sort();
  assert.equal(query('1099', ['--kind', 'Person']).hits[0].name, escapedName,
    'buffered JSON output must preserve quotes, backslashes, UTF-8 and control characters');
  assert.deepEqual(papers('Schulweg OR Haushalt AND NOT vertraulich'), [id('papers/1'), id('papers/2')]);
  assert.deepEqual(papers('(Schulweg OR Haushalt) AND NOT vertraulich'), [id('papers/2')]);
  assert.deepEqual(papers('NOT Haushalt'), [id('papers/1'), id('papers/3')]);
  assert.deepEqual(papers('NOT NOT Schulweg'), [id('papers/1')]);
  assert.deepEqual(papers('NOT (Schulweg OR NOT Haushalt)'), [id('papers/2')]);
  assert.deepEqual(papers('Schulweg AND NOT Schulweg'), []);
  assert.deepEqual(papers('Schulweg OR NOT nonexistenttoken'), [id('papers/1'), id('papers/2'), id('papers/3')]);
  const complement = query('NOT nonexistenttoken', ['--kind', 'Person', '--limit', '100']);
  assert.equal(complement.total, 1100, 'complements include unmatched records, but not unused tail bits');
  assert.equal(complement.hits.length, 100);
  assert.equal(new Set(complement.hits.map(hit => hit.id)).size, 100);
  const allPeople = [];
  for (let offset = 0; offset < 1100; offset += 100) {
    const part = query('NOT nonexistenttoken', ['--kind', 'Person', '--limit', '100', '--offset', String(offset)]);
    assert.equal(part.total, 1100);
    allPeople.push(...part.hits);
  }
  assert.equal(new Set(allPeople.map(hit => hit.id)).size, 1100, 'all result pages must be reachable without duplicates');
  assert.deepEqual(allPeople.slice(0, 100), complement.hits);
  for (const [offset, limit] of [[0, 1], [1, 7], [47, 50], [1095, 20], [1100, 50], [4294967295, 100]]) {
    const part = query('NOT nonexistenttoken', ['--kind', 'Person', '--limit', String(limit), '--offset', String(offset)]);
    assert.equal(part.total, 1100);
    assert.deepEqual(part.hits, allPeople.slice(offset, offset + limit), 'offset slices the ranked set, not record traversal order');
  }
  for (const order of ['date-desc', 'date-asc']) {
    const expected = [...allPeople].sort((a, b) => {
      if (!a.date || !b.date) return !a.date - !b.date;
      return order === 'date-desc' ? b.date.localeCompare(a.date) : a.date.localeCompare(b.date);
    });
    for (const offset of [0, 7, 24, 1095]) {
      const actual = query('NOT nonexistenttoken', ['--kind', 'Person', '--sort', order, '--limit', '50', '--offset', String(offset)]);
      assert.deepEqual(actual.hits, expected.slice(offset, offset + 50),
        'date sorting includes all matches, puts missing dates last, and keeps stable relevance ties across pages');
    }
  }
  for (const args of [['--offset', '-1'], ['--offset', '4294967296'], ['--sort', 'newest'], ['--group', 'page']])
    run(searcher, ['--index', index, ...args, 'Schulweg'], 1);
  assert.equal(query('Deleted').total, 0);
  assert.equal(query('Veraltet').total, 0, 'newer modified metadata wins across annual archives');
  const phrase = query('"sicherer schulweg"', ['--kind', 'Page']);
  assert.equal(phrase.total, 1);
  assert.equal(phrase.hits[0].page, 2, 'PDF page-tree order, not object order, defines page citations');
  assert.match(phrase.hits[0].snippet, /sicherer Schulweg/);
  assert.equal(phrase.hits[0].relatedName, 'Schulweg vertraulich', 'document results expose the title of their matching Paper');
  assert.equal(phrase.hits[0].documentUrl, file(1).downloadUrl, 'PDF hits link to downloadUrl before accessUrl');
  const page = JSON.parse(run(searcher, ['--index', index, '--show', file(1).id, '--page', '2', '--json']).stdout);
  assert.match(page.text, /sicherer Schulweg/);
  assert.equal(page.metadata.id, file(1).id);
  const paper = JSON.parse(run(searcher, ['--index', index, '--show', id('papers/1'), '--json']).stdout);
  assert.equal(paper.metadata.reference, '23/SVV/1');
  assert.equal(query('"Schulweg sicherer"', ['--kind', 'Page']).total, 0);
  assert.equal(query('"Schulweg Schulweg"', ['--kind', 'Page']).total, 0);
  assert.equal(query('Sicherheit,Schulweg', ['--kind', 'Page']).total, 1, 'unquoted punctuation-separated words use AND');
  assert.equal(query('"vertraulich 23"', ['--kind', 'Paper']).total, 0, 'phrases cannot cross metadata fields');
  assert.equal(query('schul*', ['--kind', 'Page']).total, 1);
  assert.equal(query('STRASSE', ['--kind', 'Page']).total, 1);
  assert.equal(query('größe', ['--kind', 'Page']).total, 1);
  assert.equal(query('größe', ['--kind', 'Page']).hits[0].documentUrl, file(2).accessUrl,
    'PDF hits use the original accessUrl when downloadUrl is absent');
  assert.equal(query('GRO\u0308SSE', ['--kind', 'Page']).total, 1, 'ToUnicode and decomposed German umlauts are normalized');
  const mapped = JSON.parse(run(searcher, ['--index', index, '--show', file(2).id, '--page', '1', '--json']).stdout);
  assert.match(mapped.text, /Größe ABfi😀/, 'array/scalar ranges, multicharacter mappings and UTF-16 surrogate pairs');
  assert.equal(query('"erste seite"', ['--kind', 'Page']).total, 1, 'TJ spacing creates a word boundary');
  assert.equal(query('"alpha beta"', ['--kind', 'Page']).total, 1, 'absolute text positioning separates distinct words');
  assert.equal(query('Kind', ['--kind', 'Page']).total, 1, 'adjacent text runs remain one word');
  assert.equal(query('BA', ['--kind', 'Page']).total, 1, 'q/Q restores the active PDF font');
  assert.equal(query('"November 2025"', ['--kind', 'Page']).total, 1,
    'indirect font widths keep adjacent text runs and digits together');
  assert.equal(query('Jahresabschluss', ['--kind', 'Page']).total, 1, 'line-end hyphenation is joined before indexing');
  assert.match(page.text, /Jahresabschluss 2024/);
  assert.match(page.text, /Druck-\nund Kopierkosten/, 'suspended compound hyphens are not removed');
  assert.equal(query('Amt', ['--kind', 'Page']).total, 1, 'Td moves from the text-line origin, not the previous glyph end');
  assert.equal(query('Verwaltung', ['--kind', 'Page']).total, 1, 'BT/ET boundaries do not split adjacent text runs');
  assert.match(page.text, /Nord\nSued/, 'graphics-state transforms distinguish lines with identical local coordinates');
  const grouped = query('Schulweg', ['--group', 'paper', '--sort', 'date-desc']);
  assert.equal(grouped.group, 'paper');
  assert.equal(grouped.total, 2, 'shared files produce one group for each related Paper');
  assert.deepEqual(grouped.hits.map(hit => [hit.relatedPaper, hit.matchCount]),
    [[id('papers/2'), 1], [id('papers/1'), 2]], 'matching metadata/pages are counted once per Paper');
  for (const order of ['relevance', 'date-desc', 'date-asc']) {
    const all = query('Schulweg', ['--group', 'paper', '--sort', order]).hits;
    for (const offset of [0, 1, 2]) {
      const part = query('Schulweg', ['--group', 'paper', '--sort', order, '--limit', '1', '--offset', String(offset)]);
      assert.equal(part.total, 2);
      assert.deepEqual(part.hits, all.slice(offset, offset + 1), 'grouping precedes ranking and pagination');
    }
  }
  assert.equal(query('Schulweg', ['--group', 'paper', '--year', '2023', '--paper-type', 'Antwort']).total, 0,
    'group expansion preserves the same-Paper filter invariant');
  assert.equal(query('Schulweg', ['--group', 'paper', '--year', '2024', '--paper-type', 'Antwort']).total, 1);
  assert.equal(query('NOT nonexistenttoken', ['--group', 'paper', '--kind', 'Person']).total, 1100,
    'objects without Paper relations are retained separately');
  const filters = ['--kind', 'Page', '--year', '2023', '--paper-type', 'Antrag', '--organization', 'alpha'];
  assert.equal(query('Schulweg', filters).total, 1);
  assert.equal(query('Schulweg', ['--kind', 'Page', '--year', '2024', '--paper-type', 'Antwort']).total, 1);
  const contextual = query('Schulweg', ['--kind', 'Page', '--year', '2024', '--sort', 'date-desc']).hits[0];
  assert.equal(contextual.relatedName, 'Haushalt öffentlich', 'title follows the SAME Paper as the active filters');
  assert.equal(contextual.relatedDate, '2024-01-01');
  const datedPages = query('NOT nonexistenttoken', ['--kind', 'Page', '--sort', 'date-desc']).hits;
  assert.equal(datedPages[0].relatedDate, '2024-01-01', 'PDF dates come from related Papers');
  assert.equal(datedPages.at(-1).relatedDate, '2023-01-01');
  assert.equal(query('Schulweg', ['--kind', 'Page', '--year', '2023', '--paper-type', 'Antwort']).total, 0,
    'combined filters must match the same related paper');
  for (const text of ['AND Schule', 'Schule OR', '(Schule', '"Schule', '*', 'schul*weg', 'schul**'])
    run(searcher, ['--index', index, text], 1);
  const batch = run(searcher, ['--index', index, '--batch', '--json', '--kind', 'Page'], 0,
    '"sicherer schulweg"\nGRO\u0308SSE').stdout.trim().split('\n').map(JSON.parse);
  assert.deepEqual(batch.map(result => result.total), [1, 1]);
  const wide = Array(64).fill('Schulweg').join(' OR ');
  const reusedQueries = [wide, 'NOT nonexistenttoken', 'nonexistenttoken', '"sicherer schulweg"',
    'Schulweg OR NOT Schulweg', 'Schulweg AND NOT Schulweg', wide];
  const reused = run(searcher, ['--index', index, '--batch', '--json', '--kind', 'Page'], 0,
    `${reusedQueries.join('\n')}\n`).stdout.trim().split('\n').map(JSON.parse);
  assert.deepEqual(reused.map(result => result.total), [1, 3, 0, 1, 3, 0, 1]);
  for (let i = 0; i < reusedQueries.length; i++)
    assert.deepEqual(reused[i], query(reusedQueries[i], ['--kind', 'Page']),
      'reused query buffers must preserve scores, contexts and snippets across changing atom counts');
  const benchmark = JSON.parse(run(searcher, ['--index', index, '--benchmark', '30', ...filters, 'Schulweg']).stdout);
  assert.equal(benchmark.queries, 30);
  assert.ok(benchmark.p95Us < 10000, 'fixture warm retrieval must meet the chosen latency target');
  const original = await readFile(index);
  const portable = Buffer.from(original);
  portable.fill(0, 112, 144);
  assert.equal(hash(portable), original.subarray(112, 144).toString('hex'),
    'native index SHA-256 must match an independent implementation, including header and partial blocks');
  await writeFile(join(root, 'broken.ops'), original.subarray(0, original.length - 1));
  run(searcher, ['--index', join(root, 'broken.ops'), 'Schulweg'], 1);
  const corrupt = Buffer.from(original); corrupt[corrupt.length - 8] ^= 1;
  await writeFile(join(root, 'broken.ops'), corrupt);
  run(searcher, ['--index', join(root, 'broken.ops'), 'Schulweg'], 1);
  const header = Buffer.from(original); header[16] ^= 1;
  await writeFile(join(root, 'broken.ops'), header);
  run(searcher, ['--index', join(root, 'broken.ops'), 'Schulweg'], 1);
  const strings = Number(original.readBigUInt64LE(24)), stringBytes = Number(original.readBigUInt64LE(72));
  const records = Number(original.readBigUInt64LE(32));
  for (const tail of [1, 7, 8, 9, 16]) for (const terminated of [true, false]) {
    const boundary = Buffer.from(original);
    boundary.writeUInt32LE(stringBytes - tail, records + 4);
    boundary.fill(0x78, strings + stringBytes - tail, strings + stringBytes);
    if (terminated) boundary[strings + stringBytes - 1] = 0;
    boundary.fill(0, 112, 144);
    createHash('sha256').update(boundary).digest().copy(boundary, 112);
    await writeFile(join(root, 'boundary.ops'), boundary);
    run(searcher, ['--index', join(root, 'boundary.ops'), 'nonexistenttoken'], terminated ? 0 : 1);
  }

  const bad = file(99);
  await metadata(2023, { ...bad, name: 'Broken document' });
  await writeFile(join(archives, '2023', 'documents', `${hash(bad.id)}-99.pdf`), '%PDF-1.7\nbroken\n%%EOF\n');
  run(indexer, ['--input', archives, '--output', index], 3);
  const report = (await readFile(`${index}.coverage.jsonl`, 'utf8')).trim().split('\n').map(JSON.parse);
  assert.ok(report.some(record => record.id === bad.id && record.error));
  assert.equal(query('Schulweg').incomplete, true, 'coverage gaps are part of every structured result');
  assert.equal((await readFile(join(archives, '2023', 'documents', `${hash(file(1).id)}-1.pdf`))).compare(shared), 0);
  const image = file(100);
  await metadata(2023, image);
  await writeFile(join(archives, '2023', 'documents', `${hash(image.id)}-100.pdf`), pdf([null,
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Count 1 /Kids [3 0 R] >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /XObject << /Im0 5 0 R >> /Font << /F1 6 0 R >> >> /Contents 4 0 R >>',
    stream('q 100 0 0 100 0 0 cm /Im0 Do Q BT /F1 12 Tf (1 von 1 in Zusammenstellung) Tj ET'),
    '<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8 /Length 1 >>\nstream\nx\nendstream',
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
  ]));
  await metadata(2023, file(101));
  await metadata(2023, file(102));
  await writeFile(join(archives, '2023', 'documents', `${hash(file(102).id)}-102.pdf`), pdf([null,
    '<< /Type /Catalog /Pages 2 0 R >>',
    '<< /Type /Pages /Count 1 /Kids [3 0 R] >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
    '<< /Type /Font /Subtype /Type0 /BaseFont /Test /Encoding /Identity-H /ToUnicode 6 0 R >>',
    stream('BT /F1 12 Tf <FFFF> Tj ET'),
    stream('1 beginbfchar <0001> <0041> endbfchar'),
  ]));
  run(indexer, ['--input', archives, '--output', index], 3);
  const gaps = (await readFile(`${index}.coverage.jsonl`, 'utf8')).trim().split('\n').map(JSON.parse);
  assert.ok(gaps.some(record => record.file?.endsWith('-100.pdf') && record.warnings & 4),
    'image-only pages need OCR even when a generated page stamp is extractable');
  assert.ok(gaps.some(record => record.id === file(101).id && record.error === 'document is not archived'));
  assert.ok(gaps.some(record => record.file?.endsWith('-102.pdf') && record.warnings & 1),
    'unmapped multibyte glyphs are reported without indexing beyond the font width table');
  await metadata(2023, { ...file(103), name: 'RemovedDoc', deleted: true });
  await writeFile(join(archives, '2023', 'documents', `${hash(file(103).id)}-removed.pdf`), unicode);
  const orphanHash = 'a'.repeat(64);
  await writeFile(join(archives, '2023', 'documents', `${orphanHash}-orphan.pdf`), unicode);
  await writeFile(join(archives, '2023', 'archive.json'), '{"complete":false,"referenceYear":2023}');
  run(indexer, ['--input', archives, '--output', index], 3);
  assert.equal(query('RemovedDoc').total, 0, 'deleted File binaries must not be reintroduced as orphans');
  const orphan = JSON.parse(run(searcher, ['--index', index, '--show', `urn:oparl-search:archive-file:${orphanHash}`, '--page', '1', '--json']).stdout);
  assert.equal(orphan.metadata, null, 'orphan files must not acquire invented OParl metadata');
  assert.match(orphan.text, /Größe/);
  assert.ok((await readFile(`${index}.coverage.jsonl`, 'utf8')).includes('"referenceYear":2023,"incomplete":true'));
  await metadata(2023, {
    id: id('papers/1'), type: `${schema}Paper`, reference: '23/SVV/1', name: 'Schulweg vertraulich',
    date: '2023-01-01', paperType: 'Antrag', underDirectionOf: [id('organizations/1')],
    mainFile: { ...file(1), name: 'Stale embedded name' },
    auxiliaryFile: [{ ...file(103), name: 'RemovedDoc', deleted: true }],
    subordinatedPaper: [id('papers/2')], relatedPaper: [id('papers/3'), id('papers/missing'), id('papers/deleted')],
    consultation: [
      { id: id('consultations/1'), type: `${schema}Consultation`, role: 'Stale embedded role' },
      id('consultations/2'),
      { id: id('consultations/missing'), type: `${schema}Consultation`, role: 'Nur eingebettet', organization: [id('organizations/1')] },
    ],
    web: 'https://egov.potsdam.de/public/VO020?VOLFDNR=1', modified: '2026-02-01',
  });
  await metadata(2024, { id: id('papers/2'), type: `${schema}Paper`, reference: '24/SVV/2',
    name: 'Haushalt öffentlich', date: '2024-01-01', paperType: 'Antwort',
    mainFile: file(1), underDirectionOf: [id('organizations/2')],
    superordinatedPaper: [id('papers/1')], subordinatedPaper: [id('papers/1')], modified: '2026-02-01' });
  await metadata(2023, { ...file(1), name: 'Original PDF', modified: '2026-02-02' });
  for (const [number, agendaKey] of [[1, 'agendaitem'], [2, 'agendaItem']]) {
    await metadata(2023, { id: id(`consultations/${number}`), type: `${schema}Consultation`,
      paper: id('papers/1'), [agendaKey]: id(`agendaItems/${number}`), organization: [id('organizations/2')],
      ...(number === 1 ? { meeting: id('meetings/1') } : {}), role: 'Entscheidung', authoritative: true });
    await metadata(2023, { id: id(`agendaItems/${number}`), type: `${schema}AgendaItem`, meeting: id('meetings/1'),
      consultation: id(`consultations/${number}`), name: 'Schulweg beraten', number: `5.${number}`,
      result: 'beschlossen', resolutionFile: file(2).id, auxiliaryFile: [file(1).id] });
  }
  await metadata(2023, { id: id('meetings/1'), type: `${schema}Meeting`, name: 'Test-Sitzung',
    start: '2024-04-23T18:00:00+02:00', organization: [id('organizations/1')],
    invitation: file(1).id, verbatimProtocol: file(2).id,
    agendaItem: [{ id: id('agendaItems/unrelated'), name: 'Anderer Tagesordnungspunkt' }] });
  run(indexer, ['--input', archives, '--output', index], 3);
  await testSearchWeb(searcher, index, file(1).id);
  const beforeFailure = await readFile(index);
  await writeFile(join(archives, '2023', 'metadata', 'invalid.json'), '{');
  run(indexer, ['--input', archives, '--output', index], 1);
  assert.deepEqual(await readFile(index), beforeFailure, 'a fatal rebuild preserves the published index');
  console.log('Search tests passed: positional Unicode full text, PDF page order/CMaps/Flate, metadata joins, shared-document deduplication, Boolean/phrase/prefix queries, batch mode, benchmarks, index validation and coverage gaps.');
} finally {
  await rm(root, { recursive: true, force: true });
}

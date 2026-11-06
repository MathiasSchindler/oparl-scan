import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, readFile, realpath, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createCitizenFixture, hash, id, schema } from './fixtures/citizen-questions.mjs';

const [indexer, searcher] = process.argv.slice(2).map(path => resolve(path));
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-semantics-')));
const index = join(root, 'snapshot.ops');
let snapshotId;
function run(binary, args, expected = 0) {
  const result = spawnSync(binary, args, { encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 ** 2 });
  assert.equal(result.status, expected, `${result.error ?? ''}\n${result.stderr}\n${result.stdout}`);
  return result;
}
function query(q, filters = {}, expected = 0) {
  const args = Object.entries(filters).flatMap(([key, value]) => [key, String(value)]);
  if (filters['--offset']) args.push('--snapshot-id', snapshotId);
  const result = run(searcher, ['--index', index, '--json', ...args, ...(q === undefined ? [] : [q])], expected);
  return expected ? result.stderr : JSON.parse(result.stdout);
}
try {
  const fixture = await createCitizenFixture(root);
  const store = object => writeFile(join(fixture.archive, 'metadata', `${hash(object.id)}.json`), JSON.stringify(object));
  const times = {
    a: '2026-10-25T02:30:00+02:00',
    b: '2026-10-25T02:15:00+01:00',
    ns1: '2026-10-25T02:15:00.000000001+01:00',
    ns2: '2026-10-25T02:15:00.000000002+01:00',
    otherDay: '2026-10-24T23:30:00-04:00',
    unknown: undefined,
    noZone: '2026-10-25T02:15:00',
    invalidDay: '2026-02-30T02:15:00+01:00',
  };
  for (const [name, start] of Object.entries(times))
    await store({ id: id(`meetings/m3-${name}`), type: schema + 'Meeting', name: 'M3 Chronology', start,
      meetingState: 'eingeladen' });
  for (const [name, date] of [['joint-a', '2026-01-01'], ['joint-b', '2026-09-01']])
    await store({ ...fixture.objects.find(o => o.id === id(`papers/${name}`)), date });
  await store({ id: id('consultations/m3-unique'), type: schema + 'Consultation', name: 'M3 Unique',
    meeting: id('meetings/m3-b') });
  await store({ id: id('agendaItems/m3-unique'), type: schema + 'AgendaItem', name: 'M3 Agenda',
    public: true, meeting: id('meetings/m3-b') });
  await store({ id: id('consultations/m3-ambiguous'), type: schema + 'Consultation', name: 'M3 Ambiguous',
    meeting: [id('meetings/m3-a'), id('meetings/m3-b')] });
  await store({ id: id('consultations/m3-missing'), type: schema + 'Consultation', name: 'M3 Missing',
    meeting: [id('meetings/m3-b'), id('meetings/not-indexed')] });
  await store({ id: id('files/m3-unversioned'), type: schema + 'File', name: 'M3 Unversioned',
    fileName: 'missing.pdf', downloadUrl: 'https://egov.potsdam.de/public/missing.pdf' });
  run(indexer, ['--input', fixture.archive, '--output', index], 3);
  const filters = { '--kind': 'Meeting', '--date-role': 'meeting.start' };
  const desc = query('"M3 Chronology"', { ...filters, '--sort': 'date-desc' });
  snapshotId = desc.snapshotId;
  assert.deepEqual(desc.hits.slice(0, 5).map(h => h.id),
    ['otherDay', 'ns2', 'ns1', 'b', 'a'].map(name => id(`meetings/m3-${name}`)));
  assert.ok(desc.hits.slice(5).every(h => !h.sortDate || h.sortDate.precision === 'invalid'));
  assert.equal(desc.hits[1].date, times.ns2, 'Keep the source offset and fractional seconds');
  assert.equal(desc.hits[1].sortDate.sourceObject, desc.hits[1].id);
  assert.equal(desc.hits[1].sortDate.property, 'start');
  assert.equal(desc.hits[1].sortDate.certainty, 'source-assertion');
  const interval = { ...filters, '--date-from': '2026-10-25', '--date-through': '2026-10-25', '--sort': 'date-asc' };
  const filtered = query('"M3 Chronology"', interval);
  assert.deepEqual(filtered.hits.map(h => h.id), ['a', 'b', 'ns1', 'ns2'].map(name => id(`meetings/m3-${name}`)));
  assert.equal(filtered.excludedUnknownDateRecords, 1);
  assert.equal(filtered.excludedInvalidDateRecords, 2);
  assert.equal(filtered.matchedRecords, 4);
  assert.deepEqual(filtered.hits[0].filterDate, filtered.hits[0].sortDate);
  const contradiction = query('"M3 Chronology" AND NOT Chronology', interval);
  assert.equal(contradiction.total, 0);
  assert.equal(contradiction.excludedUnknownDateRecords, 0, 'Count exclusions only after the complete Boolean expression');
  assert.equal(contradiction.excludedInvalidDateRecords, 0);
  const negative = query('NOT M3', interval);
  assert.equal(negative.total, 2);
  assert.equal(negative.excludedUnknownDateRecords, 1);
  assert.equal(negative.excludedInvalidDateRecords, 0);
  const listed = query(undefined, interval);
  assert.equal(listed.filterOnly, true);
  assert.equal(listed.total, 6, 'Include the two original DST-fold fixtures without any artificial search word');
  assert.equal(listed.excludedUnknownDateRecords, 2);
  assert.equal(listed.excludedInvalidDateRecords, 2);
  const paged = query(undefined, { ...interval, '--limit': 2, '--offset': 2 });
  assert.deepEqual(paged.hits, listed.hits.slice(2, 4), 'Pagination uses the exact same chronological order');
  assert.equal(paged.total, listed.total);
  assert.equal(paged.excludedUnknownDateRecords, listed.excludedUnknownDateRecords);
  assert.equal(query('   ', interval).total, listed.total);
  const imported = query(undefined, { '--kind': 'Meeting', '--date-role': 'importedAt', '--sort': 'date-desc' });
  assert.equal(imported.hits[0].sortDate.sourceObject, null);
  assert.equal(imported.hits[0].sortDate.sourceIndex, imported.snapshotId);
  assert.equal(imported.hits[0].sortDate.via, 'index', 'Local index import is not an OParl object date');
  assert.match(query('', {}, 1), /empty listing requires/);
  assert.match(query('*', filters, 1), /query|word|operator/i);
  assert.match(query(undefined, { '--kind': 'DefinitelyNotAnOParlType' }, 1), /unknown kind/);
  assert.match(query(undefined, { ...filters, '--date-from': '1900-02-29' }, 1), /valid YYYY/);
  assert.match(query(undefined, { ...filters, '--date-from': '2026-12-02', '--date-through': '2026-11-04' }, 1), /ordered/);
  assert.match(query(undefined, { '--kind': 'Meeting', '--date-role': 'referenceYear' }, 1), /reference year/);
  assert.match(query(undefined, { '--group': 'initiative' }, 1), /not established/);
  assert.equal(query(undefined, { ...filters, '--date-from': '2000-02-29' }).filterOnly, true);
  const unique = query('"M3 Unique"', { '--kind': 'Consultation', '--date-role': 'meeting.start',
    '--date-from': '2026-10-25' });
  assert.equal(unique.total, 1);
  assert.equal(unique.hits[0].sortDate.sourceObject, id('meetings/m3-b'));
  assert.equal(unique.hits[0].sortDate.via, 'meeting-context');
  const agenda = query('"M3 Agenda"', { '--kind': 'AgendaItem', '--date-role': 'meeting.start',
    '--date-from': '2026-10-25', '--date-through': '2026-10-25' });
  assert.equal(agenda.total, 1);
  assert.equal(agenda.hits[0].sortDate.sourceObject, id('meetings/m3-b'));
  assert.equal(agenda.hits[0].sortDate.via, 'meeting-context');
  for (const name of ['Ambiguous', 'Missing']) {
    const unknown = query(`"M3 ${name}"`, { '--kind': 'Consultation', '--date-role': 'meeting.start',
      '--date-from': '2026-10-25' });
    assert.equal(unknown.total, 0);
    assert.equal(unknown.excludedUnknownDateRecords, 1, 'Do not choose the first of several/unresolved meetings');
  }
  assert.equal(query('Radverkehr', { '--year': 2026, '--date-role': 'paper.date',
    '--date-from': '2026-08-01', '--group': 'paper' }).total, 0, 'Reference year and date must match the same direct owner');
  const joint = query('Radverkehr', { '--year': 2025, '--date-role': 'paper.date',
    '--date-from': '2026-08-01', '--group': 'paper' });
  assert.equal(joint.total, 1);
  assert.equal(joint.hits[0].relatedPaper, id('papers/joint-b'));
  assert.equal(joint.hits[0].sortDate.sourceObject, id('papers/joint-b'));
  assert.equal(joint.hits[0].sortDate.via, 'direct-paper-owner');
  assert.equal(query('Radverkehr', { '--group': 'paper' }).total, 2, 'Preserve both legitimate owners');
  for (const role of ['auto', 'paper.date']) {
    const outside = query('Radverkehr', { '--kind': 'Page', '--date-role': role, '--date-from': '2027-01-01' });
    assert.equal(outside.total, 0);
    assert.equal(outside.excludedUnknownDateRecords, 0,
      'Known out-of-range owners must not acquire an invented unknown standalone File context');
    assert.equal(outside.excludedInvalidDateRecords, 0);
  }
  const documents = query('Radverkehr', { '--group': 'document', '--kind': 'Page' });
  assert.equal(documents.total, 1, 'A shared document version is one unit, not two Papers');
  assert.equal(documents.countUnit, 'document-version');
  assert.equal(documents.hits[0].unit, 'document-version');
  assert.match(documents.hits[0].groupIdentity, /^urn:oparl-search:document:/);
  assert.match(documents.hits[0].identity, /:page:/);
  const unversioned = query('Unversioned', { '--group': 'document', '--kind': 'File' });
  assert.equal(unversioned.total, 0);
  assert.equal(unversioned.excludedUnversionedRecords, 1);
  assert.equal(query('Unversioned', { '--kind': 'File' }).total, 1, 'Keep unversioned metadata readable');
  const unversionedText = run(searcher, ['--index', index, '--group', 'document', '--kind', 'File', 'Unversioned']).stdout;
  assert.match(unversionedText, /Total=0 unit=document-version.*excludedUnversionedRecords=1/,
    'Plain CLI output must not hide why an unversioned File was excluded');
  assert.equal(query('Schulwegsicher*', { '--group': 'paper' }).hits.filter(h => h.relatedPaper).length, 1,
    'Meeting packages do not become Paper-owned content');
  const summary = run(searcher, ['--index', index, ...Object.entries(interval).flatMap(([k, v]) => [k, v])]).stdout;
  assert.equal(summary.split('Total=').length, 2, 'Print one count summary, including filter-only results');
  assert.match(summary, /excludedUnknownDateRecords=2/);
  assert.match(summary, /sortDate=.*role=meeting.start/);
  assert.ok((await readFile(index)).length > 0);
  const emptyArchive = join(root, 'empty');
  const emptyIndex = join(root, 'empty.ops');
  await mkdir(join(emptyArchive, 'metadata'), { recursive: true });
  await mkdir(join(emptyArchive, 'documents'));
  await writeFile(join(emptyArchive, 'archive.json'), JSON.stringify({ complete: true, referenceYear: 2026 }));
  run(indexer, ['--input', emptyArchive, '--output', emptyIndex]);
  const empty = JSON.parse(run(searcher, ['--index', emptyIndex, '--json', '--kind', 'Meeting']).stdout);
  assert.equal(empty.total, 0);
  assert.deepEqual(empty.hits, []);
  assert.equal(empty.filterOnly, true);
  console.log('M3 semantics passed: exact timezone/nanosecond chronology, local-day bounds, filter-only pagination, unknown/invalid dates, same-owner joins, document units and bounded typed contexts.');
} finally {
  await rm(root, { recursive: true, force: true });
}

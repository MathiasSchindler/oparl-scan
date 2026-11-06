import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, realpath, rename, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createCitizenFixture, hash, id, schema } from './fixtures/citizen-questions.mjs';

const [indexer, searcher] = process.argv.slice(2).map(path => resolve(path));
assert.ok(indexer && searcher, 'Provide native indexer and searcher paths');
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-model-')));
const index = join(root, 'snapshot.ops');
function run(binary, args, expected = 0) {
  const result = spawnSync(binary, args, { encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 ** 2 });
  assert.equal(result.status, expected, `${result.error ?? ''}\n${result.stderr}\n${result.stdout}`);
  return result;
}
const show = (object, page) => JSON.parse(run(searcher, ['--index', index, '--json', '--show', object,
  ...(page ? ['--page', String(page)] : [])]).stdout);
const query = text => JSON.parse(run(searcher, ['--index', index, '--json', '--group', 'paper', text]).stdout);

try {
  const fixture = await createCitizenFixture(root);
  const store = object => writeFile(join(fixture.archive, 'metadata', `${hash(object.id)}.json`), JSON.stringify(object));
  const system = id('system'), body = id('bodies/city');
  await store({ id: system, type: schema + 'System', body: id('bodies') });
  await store({ id: body, type: schema + 'Body', name: 'Test city', system });
  const school = fixture.objects.find(o => o.id === id('papers/school'));
  await store({ ...school, body, superordinatedPaper: [id('papers/tax-answer')] });
  await store({ ...fixture.objects.find(o => o.id === id('papers/tax-answer')),
    superordinatedPaper: [id('papers/tax'), school.id] });
  await store({ id: id('papers/suffix-only'), type: schema + 'Paper', name: 'No inferred family',
    reference: '26/SVV/0736-999', body });
  for (const [name, authoritative] of [['preliminary', false], ['unknown', undefined]]) {
    await store({ id: id(`consultations/${name}`), type: schema + 'Consultation',
      paper: school.id, authoritative });
  }
  run(indexer, ['--input', fixture.archive, '--output', index]);
  // Queries and canonical evidence must need no original metadata or PDF files.
  const hidden = join(root, 'archive-not-at-original-path');
  await rename(fixture.archive, hidden);
  const model = show(school.id).model;
  assert.deepEqual(model.context, { body, system });
  assert.equal(model.metadataOrigin, 'archive-resource');
  const contextProof = model.relations.structure.find(r => r.role === 'context.system').evidence;
  assert.deepEqual([contextProof.sourceObject, contextProof.property, contextProof.rule, contextProof.certainty],
    [body, 'system', 'body-system-context-v1', 'derived']);
  assert.equal(show(system).model.context.body, null, 'System.body is a collection, not an individual Body');
  assert.equal(show(system).model.relations.structure[0].role, 'system.bodies');
  assert.equal(model.initiative, null);
  assert.equal(model.documentRole, null, 'An OParl Paper is not automatically a request or final answer');
  assert.equal(show(id('papers/suffix-only')).model.initiative, null);
  assert.deepEqual(show(id('papers/suffix-only')).model.relations.relatedPapers, []);
  assert.deepEqual(model.relations.directSources.map(r => [r.role, r.target]),
    [['paper.mainFile', id('files/school')]]);
  assert.deepEqual(model.relations.relatedPapers.map(r => r.target), [id('papers/tax-answer')]);
  const parents = show(id('papers/tax-answer')).model.relations.relatedPapers;
  assert.deepEqual(parents.map(r => r.target), [id('papers/tax'), school.id], 'Preserve both parents and the cycle');
  assert.ok(parents.every(r => r.role === 'paper.superordinatedPaper' &&
    r.evidence.sourceObject === id('papers/tax-answer') && r.evidence.property === 'superordinatedPaper'));
  assert.equal(show(id('consultations/school')).model.relations.meetingContext[0].authority, 'authoritative');
  assert.equal(show(id('consultations/preliminary')).model.relations.meetingContext[0].authority, 'preliminary');
  assert.equal(show(id('consultations/unknown')).model.relations.meetingContext[0].authority, 'unknown');
  assert.equal(show(id('agendaItems/unrelated-0')).model.metadataOrigin, 'embedded');
  assert.equal(show(id('agendaItems/school')).model.relations.meetingContext[0].role, 'agendaItem.meeting');
  const missing = show(id('papers/request')).model.relations.relatedPapers.find(r => r.target === id('papers/not-indexed'));
  assert.ok(missing, 'Unresolved relationships remain explicit, not dropped');
  run(searcher, ['--index', index, '--show', missing.target], 1);
  const packet = show(id('files/packet'), 1);
  assert.match(packet.text, /Schulwegsicherungskonzept/);
  const papers = query('Schulwegsicher*').hits.filter(h => h.relatedPaper).map(h => h.relatedPaper);
  assert.deepEqual(papers, [school.id], 'Meeting packages and parent graphs must not create direct Paper ownership');
  assert.deepEqual(query('Radverkehr').hits.map(h => h.relatedPaper).sort(),
    [id('papers/joint-a'), id('papers/joint-b')].sort(), 'Keep every legitimate direct owner');
  const file = show(id('files/shared'));
  const page = show(id('files/shared'), 1);
  assert.equal(file.model.documentVersion.sha256, hash(fixture.documents.find(d => d.id === file.id).bytes));
  assert.equal(show(file.model.documentVersion.id).id, file.id);
  assert.equal(show(file.model.documentVersion.id, 1).model.identity, page.model.identity);
  assert.equal(show(page.model.identity).text, page.text);
  const section = page.model.sections[0];
  assert.equal(show(section.id).text, page.text);
  assert.equal(section.byteStart, 0);
  assert.equal(section.byteLength, Buffer.byteLength(page.text));
  assert.equal(section.evidence.sourceObject, file.id);
  assert.equal(section.evidence.documentVersion, file.model.documentVersion.id);
  assert.equal(section.evidence.page, 1);
  assert.equal(section.evidence.section, section.id);
  assert.equal(section.evidence.rule, 'pdf-text-v1');
  assert.equal(section.evidence.certainty, 'derived', 'PDF text is extracted, not a verified transcription');
  const meeting = show(id('meetings/late')).model;
  assert.equal(meeting.dates['meeting.start'].value, fixture.starts.late);
  assert.equal(meeting.dates['meeting.start'].evidence.property, 'start');
  assert.equal(meeting.dates['meeting.start'].evidence.certainty, 'source-assertion');
  assert.equal(meeting.dates.decidedAt, null, 'Scheduled time is not a decision or proof of occurrence');
  assert.equal(meeting.events[0].dateRole, 'meeting.start');
  assert.equal(show(id('meetings/unknown')).model.dates['meeting.start'], null);
  const request = show(id('papers/request')).model;
  assert.equal(request.dates['paper.date'].value, '2026-06-30');
  for (const name of ['document.date', 'submittedAt', 'decidedAt', 'publishedAt', 'effectiveAt', 'observedAt'])
    assert.equal(request.dates[name], null, `${name} must not be inferred from Paper.date or filesystem mtime`);
  assert.ok(request.dates.importedAt.epochSeconds > 0);
  assert.equal(request.dates.referenceYear.value, '2026');
  assert.equal(request.dates.referenceYear.evidence.property, 'reference');
  assert.equal(request.dates.referenceYear.evidence.rule, 'reference-prefix-YY-v1');
  assert.equal(show(id('files/request')).model.dates.created.value, '2000-01-01T00:00:00+01:00');
  assert.equal(show(id('files/request')).model.dates['document.date'], null);

  const original = await readFile(index), checksum = Buffer.from(original);
  checksum.fill(0, 112, 144);
  assert.equal(hash(checksum), original.subarray(112, 144).toString('hex'));
  assert.equal(model.snapshotId, hash(checksum), 'Snapshot identity covers every model section and the header');
  assert.equal(original.readUInt32LE(8), 2);
  const header = {
    records: Number(original.readBigUInt64LE(32)), owners: Number(original.readBigUInt64LE(56)),
    relations: Number(original.readBigUInt64LE(144)), facts: Number(original.readBigUInt64LE(152)),
    evidence: Number(original.readBigUInt64LE(160)), versions: Number(original.readBigUInt64LE(168)),
    sections: Number(original.readBigUInt64LE(176)),
  };
  const recordNumber = object => {
    const strings = Number(original.readBigUInt64LE(24)), count = Number(original.readBigUInt64LE(80));
    for (let n = 0; n < count; n++) {
      const start = strings + original.readUInt32LE(header.records + n * 88);
      if (original.toString('utf8', start, original.indexOf(0, start)) === object) return n;
    }
    throw new Error(`Fixture record missing: ${object}`);
  };
  const taxRecord = recordNumber(id('papers/tax'));
  const noFilesRecord = recordNumber(id('papers/suffix-only'));
  const relationRow = role => {
    const count = Number(original.readBigUInt64LE(184));
    for (let n = 0; n < count; n++)
      if (original.readUInt32LE(header.relations + n * 24 + 8) === role) return header.relations + n * 24;
    throw new Error(`Fixture relation missing: ${role}`);
  };
  for (const mutate of [
    b => b.writeBigUInt64LE(0n, 224),
    b => b.writeUInt32LE(0xffffffff, header.sections + 12),
    b => b.writeUInt32LE(0xffffffff, header.relations + 8),
    b => b.writeUInt32LE(0xffffffff, header.relations + 12),
    b => b.writeUInt32LE(0xffffffff, header.facts + 8),
    b => b.writeUInt32LE(0xffffffff, header.evidence),
    b => b.writeUInt32LE(taxRecord, header.versions),
    b => b.writeUInt32LE(noFilesRecord, header.owners + 4),
    b => b.writeUInt32LE(1, relationRow(5) + 8), // A Meeting attachment cannot become Paper.mainFile.
    b => b.writeUInt32LE(original.readUInt32LE(header.records + taxRecord * 88), relationRow(24) + 4),
  ]) {
    const broken = Buffer.from(original);
    mutate(broken);
    broken.fill(0, 112, 144);
    Buffer.from(hash(broken), 'hex').copy(broken, 112);
    await writeFile(join(root, 'broken.ops'), broken);
    run(searcher, ['--index', join(root, 'broken.ops'), 'Schulweg'], 1);
  }
  const legacy = Buffer.alloc(144);
  legacy.write('OPARLIDX'); legacy.writeUInt32LE(1, 8);
  await writeFile(join(root, 'old.ops'), legacy);
  assert.match(run(searcher, ['--index', join(root, 'old.ops'), 'x'], 1).stderr, /rebuild with oparl-index/);
  await rename(hidden, fixture.archive);
  run(indexer, ['--input', fixture.archive, '--output', index]);
  assert.equal(show(file.id).model.documentVersion.id, file.model.documentVersion.id);
  assert.equal(show(file.id, 1).model.identity, page.model.identity);
  assert.equal(show(file.id, 1).model.sections[0].id, section.id);
  assert.equal(show(id('meetings/late')).model.events[0].id, meeting.events[0].id);
  const shared = fixture.documents.find(d => d.id === file.id);
  await writeFile(join(fixture.archive, 'documents', `${hash(file.id)}-fixture.pdf`), Buffer.concat([shared.bytes, Buffer.from('\n')]));
  run(indexer, ['--input', fixture.archive, '--output', index]);
  assert.notEqual(show(file.id).model.documentVersion.id, file.model.documentVersion.id);
  assert.notEqual(show(file.id, 1).model.identity, page.model.identity);
  assert.notEqual(show(file.id, 1).model.sections[0].id, section.id);
  run(searcher, ['--index', index, '--show', file.model.documentVersion.id], 1);
  assert.equal(show(id('files/packet'), 1).model.identity, packet.model.identity, 'Unchanged document identities stay stable');
  console.log('Canonical model tests passed: typed direct/context graphs, multiple parents/cycles, explicit unknowns, provenance, immutable citations, archive-free reads and format integrity.');
} finally {
  await rm(root, { recursive: true, force: true });
}

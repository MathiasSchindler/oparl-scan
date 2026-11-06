import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { mkdtemp, realpath, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';
import { createCitizenFixture, hash, id, schema } from './fixtures/citizen-questions.mjs';

const [indexer, searcher, reportPath] = process.argv.slice(2).map(p => resolve(p));
const root = await realpath(await mkdtemp(join(tmpdir(), 'oparl-research-')));
const index = join(root, 'index.ops');
let child, stopped;
function cli(args, expected = 0) {
  const r = spawnSync(searcher, ['--index', index, '--json', ...args],
    { encoding: 'utf8', timeout: 30000, maxBuffer: 32 * 1024 ** 2 });
  assert.equal(r.status, expected, `${r.error ?? ''}\n${r.stderr}\n${r.stdout}`);
  return expected ? r : JSON.parse(r.stdout);
}
try {
  const fixture = await createCitizenFixture(root);
  const store = object => writeFile(join(fixture.archive, 'metadata', hash(object.id) + '.json'), JSON.stringify(object));
  const person = id('people/author'), council = id('organizations/council');
  await store({ id: person, type: schema + 'Person', name: 'Fixture author' });
  const schoolAgenda = fixture.objects.find(o => o.id === id('agendaItems/school'));
  await store({ ...schoolAgenda, order: 20, number: '2', public: true, result: 'angenommen', resolutionFile: id('files/school') });
  await store({ ...fixture.objects.find(o => o.id === id('agendaItems/tax')), order: 10, public: true });
  const late = fixture.objects.find(o => o.id === id('meetings/late'));
  await store({ ...late, agendaItem: [...late.agendaItem, id('agendaItems/missing'), {
    id: id('agendaItems/nonpublic'), type: schema + 'AgendaItem', public: false, name: 'Nonpublic TOP',
  }] });
  await store({ id: id('meetings/held'), type: schema + 'Meeting', meetingState: 'durchgefuehrt' });
  await store({ id: id('meetings/cancelled'), type: schema + 'Meeting', cancelled: true, meetingState: 'eingeladen' });
  const files = Array.from({ length: 36 }, (_, n) => id('files/bulk-' + n));
  const bytes = fixture.documents[0].bytes;
  for (const [n, file] of files.entries()) {
    await store({ id: file, type: schema + 'File', name: 'Anlage ' + n, fileName: n + '.pdf',
      downloadUrl: `https://ris.test/pdf/${n}.pdf` });
    await writeFile(join(fixture.archive, 'documents', hash(file) + '-fixture.pdf'), bytes);
  }
  await store({ id: id('papers/large'), type: schema + 'Paper', name: 'Large paper',
    description: 'Long raw description. '.repeat(400), auxiliaryFile: files, originatorPerson: [person],
    underDirectionOf: [id('organizations/alpha')], paperType: 'Antrag', reference: '26/SVV/9999' });
  await store({ id: id('consultations/incoming'), type: schema + 'Consultation',
    paper: id('papers/large'), authoritative: false });
  const built = spawnSync(indexer, ['--input', fixture.archive, '--output', index],
    { encoding: 'utf8', timeout: 60000 });
  assert.equal(built.status, 0, built.stderr);
  const reservation = createServer();
  await new Promise((accept, reject) => { reservation.once('error', reject); reservation.listen(0, '127.0.0.1', accept); });
  const port = reservation.address().port;
  await new Promise((accept, reject) => reservation.close(error => error ? reject(error) : accept()));
  child = spawn(searcher, ['--index', index, '--serve', '--port', String(port)], { stdio: ['ignore', 'ignore', 'pipe'] });
  let logs = '', launchError;
  child.stderr.setEncoding('utf8').on('data', data => { logs += data; });
  child.on('error', error => { launchError = error; });
  stopped = new Promise(accept => child.once('close', accept));
  const base = `http://127.0.0.1:${port}`;
  async function api(path, args = {}, expected = 200) {
    const response = await fetch(base + path + '?' + new URLSearchParams(args), { signal: AbortSignal.timeout(8000) });
    const data = await response.json();
    assert.equal(response.status, expected, JSON.stringify(data));
    return data;
  }
  const deadline = Date.now() + 15000;
  for (;;) {
    if (launchError) throw launchError;
    assert.equal(child.exitCode, null, logs);
    try { await api('/api/status'); break; }
    catch (error) {
      if (error.cause?.code !== 'ECONNREFUSED' || Date.now() >= deadline) throw error;
      await delay(25);
    }
  }
  let sequence = 0;
  async function rpc(name, args, modern = false, expectError = false) {
    const protocol = modern ? '2026-07-28' : '2025-11-25';
    const params = { name, ...(args === undefined ? {} : { arguments: args }),
      ...(modern ? { _meta: { 'io.modelcontextprotocol/protocolVersion': protocol,
        'io.modelcontextprotocol/clientCapabilities': {} } } : {}) };
    const response = await fetch(base + '/mcp', { method: 'POST', signal: AbortSignal.timeout(8000),
      headers: { 'Content-Type': 'application/json', Accept: 'application/json', 'MCP-Protocol-Version': protocol,
        ...(modern ? { 'Mcp-Method': 'tools/call', 'Mcp-Name': name } : {}) },
      body: JSON.stringify({ jsonrpc: '2.0', id: ++sequence, method: 'tools/call', params }) });
    const body = await response.text(), result = JSON.parse(body).result;
    assert.equal(response.status, 200, body);
    assert.equal(result.isError, expectError, body);
    if (!expectError) assert.deepEqual(JSON.parse(result.content[0].text), result.structuredContent);
    return { data: result.structuredContent, bytes: Buffer.byteLength(body) };
  }
  const status = await api('/api/status'), snapshotId = status.snapshotId;
  assert.deepEqual(status, cli(['--status']));
  assert.ok(status.coverage.perType.find(t => t.kind === 'Paper').unresolvedRelationTargets > 0);
  const bodySizes = [];
  for (const modern of [false, true]) {
    for (const paper of [id('papers/school'), id('papers/request'), id('papers/large')]) {
      const { data, bytes } = await rpc('get_paper', { id: paper }, modern);
      bodySizes.push(bytes);
      assert.ok(bytes < 20000, `actual HTTP body ${bytes} must be below 20,000 bytes`);
      assert.deepEqual(data, cli(['--paper', paper]));
      assert.deepEqual(data, await api('/api/paper', { id: paper }));
      if (paper.endsWith('/large')) {
        assert.ok(data.next);
        assert.ok(data.objects[0].omittedFields.includes('description'));
        assert.ok(data.objects.find(o => o.id === id('consultations/incoming'))
          .relationshipPath.some(p => p.direction === 'incoming' && p.role === 'consultation.paper'));
      }
    }
  }
  async function collect(path, args) {
    let data = await api(path, args);
    const all = [...data.objects], total = data.total;
    while (data.next) {
      const n = data.next;
      data = await api(path, { ...args, offset: n.offset, limit: n.limit, snapshotId: n.snapshotId });
      assert.ok(data.returned > 0);
      all.push(...data.objects);
    }
    assert.equal(all.length, total);
    assert.equal(new Set(all.map(o => o.id)).size, total);
    return all;
  }
  const objects = await collect('/api/paper', { id: id('papers/large') });
  assert.equal(objects.length, files.length + 3, 'paper, incoming consultation, organization and every file');
  assert.ok(files.every(file => objects.some(o => o.id === file && o.citation.documentVersion?.sha256 === hash(bytes))));
  const request = await collect('/api/paper', { id: id('papers/request') });
  const missing = request.find(o => o.id === id('papers/not-indexed'));
  assert.equal(missing.indexed, false);
  assert.equal(missing.metadata, null);
  assert.equal(request.find(o => o.id === id('files/request')).metadata.fileName, 'request.pdf');
  const bundle = (await collect('/api/paper', { id: id('papers/school') }))
    .find(o => o.kind === 'Consultation').consultation;
  assert.equal(bundle.authority, 'authoritative');
  assert.ok(bundle.context.some(c => c.object.metadata?.start === fixture.starts.late));
  assert.ok(bundle.context.some(c => c.object.id === council && c.role === 'meeting.organization'));
  assert.ok(bundle.context.some(c => c.object.metadata?.result === 'angenommen'));
  assert.equal(bundle.decisionSources[0].sourceAgenda, schoolAgenda.id);
  assert.equal(bundle.decisionSources[0].documentVersion.sha256, hash(bytes));
  const raw = await api('/api/object', { id: id('papers/large'), raw: true });
  assert.ok(raw.metadata.description.length > 8000);
  const projected = await api('/api/object', { id: id('papers/large'), fields: 'name' });
  assert.deepEqual(projected.metadata, { name: 'Large paper' });
  assert.deepEqual(projected, cli(['--show', id('papers/large'), '--fields', 'name']));
  await api('/api/object', { id: id('papers/large'), raw: true, fields: 'name' }, 400);
  await api('/api/object', { id: id('papers/large'), offset: 1, snapshotId }, 400);
  await api('/api/paper', { id: id('papers/large'), offset: 1 }, 400);
  await api('/api/paper', { id: id('papers/large'), offset: 1, snapshotId: 'changed' }, 409);
  await api('/api/object', { id: missing.id }, 404);
  await rpc('get_paper', { id: id('papers/large'), offset: 1, snapshotId: 'changed' }, true, true);
  const agenda = await api('/api/agenda', { id: late.id });
  assert.equal(agenda.nonPublicReferences, 1);
  assert.equal(agenda.unresolvedAgendaReferences, 1);
  assert.equal(agenda.publicStatusUnspecified, 128);
  const tops = await collect('/api/agenda', { id: late.id });
  assert.equal(tops.length, 131);
  assert.deepEqual(tops.slice(0, 2).map(o => o.metadata.order), [10, 20]);
  assert.ok(!tops.some(o => o.metadata?.name === 'Nonpublic TOP'));
  for (const [name, lifecycle] of [['late', 'planned'], ['held', 'held'], ['cancelled', 'cancelled'], ['unknown', 'planned']]) {
    const data = await api('/api/meeting', { id: id('meetings/' + name) });
    assert.equal(data.objects[0].lifecycle, lifecycle);
    assert.deepEqual(data, cli(['--meeting', id('meetings/' + name)]));
    assert.deepEqual((await rpc('get_meeting', { id: data.id }, true)).data, data);
  }
  const options = await api('/api/options', { kind: 'Person', fields: 'name' });
  assert.ok(options.paperTypes.includes('Kleine Anfrage'));
  assert.ok(options.projectionFields.includes('downloadUrl'));
  assert.ok(options.dateRoles.includes('meeting.start'));
  assert.equal(options.objects[0].id, person);
  assert.deepEqual((await rpc('get_search_options', { kind: 'Person', fields: 'name' }, true)).data, options);
  assert.deepEqual((await rpc('get_search_options', undefined)).data, cli(['--options']));
  const owned = cli(['--kind', 'Paper', '--actor-id', person]);
  assert.deepEqual(owned.hits.map(h => h.id), [id('papers/large')]);
  assert.deepEqual(await api('/api/search', { kind: 'Paper', actorId: person }), owned);
  assert.equal(cli(['--kind', 'Page', '--year', '2026', '--organization-id', id('organizations/beta'), 'Radverkehr']).total, 0);
  assert.ok(cli(['--kind', 'Page', '--year', '2026', '--organization-id', id('organizations/alpha'), 'Radverkehr'])
    .hits.every(h => h.relatedPaper === id('papers/joint-a')));
  let singleBytes = 0, bulkBytes = 0, bulkCalls = 0;
  const batch = files.slice(0, 20);
  for (const file of batch) singleBytes += (await rpc('get_object', { id: file, raw: true })).bytes;
  let n, fetched = [];
  do {
    const args = { ids: batch, fields: 'name,fileName,downloadUrl', ...(n ? { offset: n.offset, snapshotId: n.snapshotId } : {}) };
    const result = await rpc('get_objects', args, true);
    assert.ok(result.bytes < 20000);
    bulkBytes += result.bytes; bulkCalls++; fetched.push(...result.data.objects); n = result.data.next;
    if (n) assert.deepEqual(n.retainArguments, ['ids']);
  } while (n);
  assert.equal(fetched.length, batch.length);
  assert.ok(bulkCalls < batch.length && bulkBytes < singleBytes);
  assert.ok(fetched.every(o => o.citation.documentVersion.sha256 === hash(bytes)));
  const rawBatch = await rpc('get_objects', { ids: batch.slice(0, 2), raw: true, fields: 'name', limit: 1 });
  assert.equal(rawBatch.data.next.raw, true);
  const rawContinuation = await rpc('get_objects', { ids: batch.slice(0, 2), raw: true, fields: 'name',
    limit: 1, offset: rawBatch.data.next.offset, snapshotId: rawBatch.data.next.snapshotId });
  assert.equal(rawContinuation.data.next, null);
  assert.equal(rawContinuation.data.objects[0].id, batch[1]);
  const report = { milestone: 'M4', passed: true, bodySizes, maxStandardPaperBodyBytes: Math.max(...bodySizes),
    continuationObjects: objects.length, agendaReferences: tops.length,
    bulk: { objects: batch.length, singleCalls: batch.length, bulkCalls, singleBytes, bulkBytes } };
  if (reportPath) await writeFile(reportPath, JSON.stringify(report, null, 2) + '\n');
  console.log('M4 research passed: budgets, complete continuations, typed decision evidence, raw/projection, bulk, IDs, lifecycle and snapshots.');
  console.log(JSON.stringify(report));
} finally {
  if (child?.pid && child.exitCode === null) { child.kill('SIGTERM'); await stopped; }
  await rm(root, { recursive: true, force: true });
}

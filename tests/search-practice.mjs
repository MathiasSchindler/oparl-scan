import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';

const [base = 'http://127.0.0.1:8080', reportPath] = process.argv.slice(2);
const deadline = Date.now() + 120000;
let sequence = 0;
async function rpc(name, args, modern = false) {
  assert.ok(Date.now() < deadline, 'practice run exceeded its two-minute deadline');
  const protocol = modern ? '2026-07-28' : '2025-03-26';
  const response = await fetch(base + '/mcp', {
    method: 'POST', signal: AbortSignal.timeout(10000),
    headers: { 'Content-Type': 'application/json', Accept: 'application/json', 'MCP-Protocol-Version': protocol,
      ...(modern ? { 'Mcp-Method': 'tools/call', 'Mcp-Name': name } : {}) },
    body: JSON.stringify({ jsonrpc: '2.0', id: ++sequence, method: 'tools/call',
      params: { name, arguments: args, ...(modern ? { _meta: {
        'io.modelcontextprotocol/protocolVersion': protocol, 'io.modelcontextprotocol/clientCapabilities': {},
      } } : {}) } }),
  });
  const body = await response.text(), result = JSON.parse(body).result;
  assert.equal(response.status, 200, body);
  assert.equal(result?.isError, false, body);
  if (!modern) assert.deepEqual(JSON.parse(result.content[0].text), result.structuredContent);
  return { data: result.structuredContent, bytes: Buffer.byteLength(body) };
}
async function collect(name, original, modern = false, property = 'objects') {
  let args = original, first, last, bytes = 0, calls = 0, maxBytes = 0;
  const objects = [];
  do {
    const result = await rpc(name, args, modern);
    first ??= result; last = result;
    assert.ok(result.bytes < 20000, `${name}: actual HTTP body ${result.bytes} exceeds 20,000 bytes`);
    bytes += result.bytes; calls++; maxBytes = Math.max(maxBytes, result.bytes);
    objects.push(...result.data[property]);
    if (result.data.next) {
      const { operation, retainArguments, ...cursor } = result.data.next;
      assert.equal(operation, name);
      args = { ...original, ...cursor };
    }
  } while (last.data.next);
  assert.equal(objects.length, first.data.total);
  const ids = objects.map(o => o.id ?? [o.source, o.role, o.target].join('|'));
  assert.equal(new Set(ids).size, ids.length, 'pagination must not duplicate or lose objects/relationships');
  return { first: first.data, objects,
    metrics: { firstBytes: first.bytes, bytes, calls, maxBytes, total: objects.length } };
}
const status = (await rpc('get_index_status', {})).data;
const meetings = await rpc('search', { q: 'Stadtverordnetenversammlung', kind: 'Meeting',
  dateRole: 'meeting.start', dateFrom: '2026-01-01', dateThrough: '2026-12-31', sort: 'date-desc', limit: 5 });
assert.equal(meetings.data.hits.length, 5);
assert.ok(meetings.bytes < 25000, 'five meeting hits must not serialize their entire incoming neighborhoods');
let incomingTotal = 0;
for (const hit of meetings.data.hits) {
  assert.equal(hit.evidence.relationshipPath.length, 0);
  const { operation, ...args } = hit.evidence.relationsAvailable;
  const relations = await collect(operation, { ...args, limit: 100 }, true, 'relationships');
  assert.equal(relations.objects.length, hit.evidence.relationshipTotal);
  incomingTotal += relations.objects.length;
}
const report = { snapshotId: status.snapshotId, indexMemoryBytes: status.indexMemoryBytes,
  meetings: { bytes: meetings.bytes, hits: 5, fullyReloadedIncomingRelations: incomingTotal }, legacy: {}, modern: {} };
for (const modern of [false, true]) {
  const target = modern ? report.modern : report.legacy;
  const paper = await collect('get_paper', { id: 'https://www.potsdam.sitzung-online.de/oparl/papers/2008128' }, modern);
  const agenda = await collect('get_agenda', { id: 'https://www.potsdam.sitzung-online.de/oparl/meetings/2001592' }, modern);
  const metadata = new Map(paper.objects.map(o => [o.id, o]));
  for (const node of paper.objects) {
    if (node.parentId) assert.ok(metadata.has(node.parentId));
    if (node.kind === 'File' && node.citation.documentVersion)
      assert.match(node.citation.documentVersion.sha256, /^[0-9a-f]{64}$/);
  }
  assert.equal(agenda.first.meeting.lifecycle, 'planned');
  assert.equal(agenda.first.meeting.statusConflict, true);
  assert.ok(agenda.first.meeting.agendaItemsWithResult > 0);
  target.paper = paper.metrics; target.agenda = agenda.metrics;
  const ids = paper.objects.filter(o => o.indexed).slice(0, 20).map(o => o.id);
  assert.equal(ids.length, 20);
  const fields = 'name,fileName,downloadUrl';
  let singleBytes = 0;
  const expected = new Map();
  for (const id of ids) {
    const single = await rpc('get_object', { id, fields }, modern);
    singleBytes += single.bytes; expected.set(id, single.data.metadata);
  }
  const bulk = await collect('get_objects', { ids, fields }, modern);
  for (const node of bulk.objects) assert.deepEqual(node.metadata, expected.get(node.id));
  assert.ok(bulk.metrics.bytes < singleBytes);
  target.bulk = { ...bulk.metrics, singleCalls: 20, singleBytes };
}
if (status.snapshotId === 'ded8e229c72c61e3c726387bd2409a81fa3d7fa8345046104545ac6549dd4955') {
  assert.equal(incomingTotal, 666);
  assert.equal(report.legacy.paper.total, 60);
  assert.equal(report.legacy.agenda.total, 109);
  assert.ok(report.legacy.paper.bytes < 232370 && report.legacy.paper.calls < 25);
  assert.ok(report.legacy.agenda.bytes < 320292 && report.legacy.agenda.calls < 29);
}
if (reportPath) await writeFile(reportPath, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));

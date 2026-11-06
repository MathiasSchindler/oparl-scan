import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { once } from 'node:events';
import { stat } from 'node:fs/promises';
import { createServer, connect } from 'node:net';
import { setTimeout as delay } from 'node:timers/promises';
import { Script } from 'node:vm';
import { testSearchMcp } from './search-mcp.mjs';

export async function testSearchWeb(binary, index, fileId) {
  if (process.platform === 'win32') {
    console.log('Web tests skipped: pinned newos has no Windows TCP listener backend.');
    return;
  }
  const reservation = createServer();
  await new Promise(resolve => reservation.listen(0, '127.0.0.1', resolve));
  const port = reservation.address().port;
  await new Promise(resolve => reservation.close(resolve));
  const base = `http://127.0.0.1:${port}`;
  const child = spawn(binary, ['--index', index, '--serve', '--port', String(port)],
    { stdio: ['ignore', 'pipe', 'pipe'] });
  let log = '', launchError, stdout = '', idle;
  const held = [];
  child.stderr.setEncoding('utf8').on('data', data => { log += data; });
  child.stdout.setEncoding('utf8').on('data', data => { stdout += data; });
  child.on('error', error => { launchError = error; });
  const get = (path, options = {}) => fetch(base + path, { signal: AbortSignal.timeout(8000), ...options });
  const cli = args => {
    const result = spawnSync(binary, ['--index', index, '--json', ...args],
      { encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024 });
    assert.equal(result.status, 0, `${result.error ?? ''}\n${result.stderr}`);
    return result.stdout;
  };
  const raw = request => new Promise((resolve, reject) => {
    const socket = connect(port, '127.0.0.1');
    let response = '';
    socket.setEncoding('utf8');
    socket.setTimeout(8000, () => socket.destroy(new Error('raw HTTP request timed out')));
    socket.on('data', data => { response += data; });
    socket.once('error', reject);
    socket.once('end', () => resolve(response));
    socket.once('connect', () => socket.write(request));
  });
  let snapshotId;
  try {
    const deadline = Date.now() + 8000;
    for (;;) {
      if (launchError) throw launchError;
      assert.equal(child.exitCode, null, log);
      try {
        const response = await get('/api/status');
        assert.equal(response.status, 200);
        const status = await response.json();
        snapshotId = status.snapshotId;
        assert.ok(status.records > 1100 && status.terms > 0);
        assert.equal(status.incomplete, true);
        assert.equal(status.indexBytes, (await stat(index)).size, 'health reports the loaded index file size in bytes');
        assert.ok(status.indexMemoryBytes > status.indexBytes);
        assert.ok(status.pages > 0 && status.warningPages > 0 && status.ocrPages > 0);
        assert.ok(status.ocrPages <= status.warningPages && status.warningPages <= status.pages);
        break;
      } catch (error) {
        if (error.cause?.code !== 'ECONNREFUSED' || Date.now() >= deadline) throw error;
        await delay(25);
      }
    }
    const page = await get('/');
    assert.equal(page.status, 200);
    assert.match(page.headers.get('content-type'), /text\/html/);
    assert.match(page.headers.get('content-security-policy'), /script-src 'self'/);
    assert.equal(page.headers.get('access-control-allow-origin'), null);
    const html = await page.text();
    assert.match(html, /<html lang="de">/);
    assert.match(html, /href="\/health"/);
    assert.doesNotMatch(html, /id="coverage"|Index ist unvollstaendig/);
    const health = await get('/health');
    assert.equal(health.status, 200);
    assert.match(await health.text(), /id="health-stats"/);
    const headHealth = await get('/health', { method: 'HEAD' });
    assert.equal(headHealth.status, 200);
    assert.equal(await headHealth.text(), '');
    const paperId = fileId.replace('/files/1', '/papers/1');
    const origin = paperId.slice(0, paperId.lastIndexOf('/') + 1).replace('/papers/', '/');
    const objectId = path => origin + path;
    const caseParameters = new URLSearchParams({ id: paperId });
    const casePage = await get('/paper?' + caseParameters);
    assert.equal(casePage.status, 200);
    assert.match(casePage.headers.get('content-type'), /text\/html/);
    assert.match(await casePage.text(), /id="case-consultations"/);
    const caseResponse = await get('/api/paper?' + caseParameters);
    assert.equal(caseResponse.status, 200);
    const context = await caseResponse.json();
    let next = context.next;
    while (next) {
      const part = await (await get('/api/paper?' + new URLSearchParams({ id: paperId,
        offset: String(next.offset), limit: String(next.limit), snapshotId: next.snapshotId }))).json();
      context.objects.push(...part.objects); next = part.next;
    }
    assert.equal(context.id, paperId);
    assert.equal(context.incomplete, true);
    const caseObjects = new Map(context.objects.map(object => [object.id, object]));
    assert.equal(caseObjects.size, context.objects.length, 'cycles and repeated relations must not duplicate objects');
    assert.equal(caseObjects.get(fileId).metadata.name, 'Original PDF', 'canonical indexed metadata wins over stale embedded metadata');
    assert.equal(caseObjects.get(objectId('consultations/1')).metadata.role, 'Entscheidung');
    for (const path of ['papers/2', 'papers/3', 'agendaItems/1', 'agendaItems/2', 'meetings/1', 'organizations/1', 'organizations/2'])
      assert.equal(caseObjects.get(objectId(path))?.indexed, true, path);
    assert.equal(caseObjects.has(objectId('agendaItems/unrelated')), false, 'do not expand the entire meeting agenda into this case');
    for (const path of ['papers/missing', 'papers/deleted'])
      assert.deepEqual([caseObjects.get(objectId(path)).indexed, caseObjects.get(objectId(path)).metadata], [false, null]);
    const embedded = caseObjects.get(objectId('consultations/missing'));
    assert.equal(embedded.indexed, true, 'canonical snapshots also index typed embedded objects');
    assert.equal(embedded.metadata.role, 'Nur eingebettet', 'retain inline details when a standalone resource is unavailable');
    const rawContext = await (await get('/api/paper?' + new URLSearchParams({ id: paperId, raw: 'true' }))).json();
    assert.equal(rawContext.objects.find(o => o.id === objectId('files/103')).metadata.deleted, true,
      'explicit raw context retains deleted inline source metadata');
    for (const route of ['/paper', '/api/paper']) {
      const response = await get(route + '?' + caseParameters, { method: 'HEAD' });
      assert.equal(response.status, 200);
      assert.equal(await response.text(), '');
    }
    const script = await get('/app.js');
    assert.match(script.headers.get('content-type'), /text\/javascript/);
    const scriptText = await script.text();
    new Script(scriptText);
    const presentation = new Script(scriptText.slice(0, scriptText.indexOf('async function health(')) +
      scriptText.slice(scriptText.indexOf('function renderHit('), scriptText.indexOf('async function search(params)')) +
      '\n({hitTitle,renderHit,renderPdfPage})').runInNewContext({
      URLSearchParams, TextEncoder, TextDecoder,
      document: { createElement: tag => ({ tagName: tag.toUpperCase(), children: [], dataset: {}, listeners: {},
        append(...items) { this.children.push(...items); },
        addEventListener(name, listener) { this.listeners[name] = listener; } }) },
    });
    const descendants = node => [node, ...node.children.flatMap(descendants)];
    const contextTitle = (name, sourceKind, sourceName) => presentation.hitTitle({ name, kind: 'Page',
      fileName: 'Original filename.pdf', evidence: { relationshipPath: [{ source: 'context/1', sourceKind, sourceName }] } });
    assert.equal(contextTitle('Sammeldokument \u00f6ffentlich', 'AgendaItem', 'Fazit zur Geothermie'), 'Fazit zur Geothermie');
    assert.equal(contextTitle('Protokoll \u00f6ffentlich', 'Meeting', '32. Sitzung des Hauptausschusses'),
      'Protokoll: 32. Sitzung des Hauptausschusses');
    assert.equal(contextTitle('Alle Anlagen oeffentlich', 'Meeting', 'Stadtverordnetenversammlung'),
      'Anlagen: Stadtverordnetenversammlung');
    assert.equal(presentation.hitTitle({ name: 'Sammeldokument \u00f6ffentlich', fileName: 'Archive.pdf',
      evidence: { relationshipPath: [{ source: 'a', sourceKind: 'AgendaItem', sourceName: 'First TOP' },
        { source: 'b', sourceKind: 'AgendaItem', sourceName: 'Different TOP' }] } }), 'Archive.pdf',
    'ambiguous context must not pick an arbitrary agenda title');
    assert.equal(presentation.hitTitle({ name: 'Protokoll \u00f6ffentlich', fileName: 'Minutes.pdf' }), 'Minutes.pdf');
    assert.equal(presentation.hitTitle({ name: 'Sammeldokument \u00f6ffentlich', relatedName: 'Actual paper title' }), 'Actual paper title');
    for (const evidence of [
      { relationshipsComplete: false, relationshipTotal: 6,
        relationshipPath: [{ source: 'a', sourceKind: 'Meeting', sourceName: 'Visible meeting' }] },
      { relationshipsComplete: true, relationshipPath: [
        { source: 'a', sourceKind: 'Meeting', sourceName: 'Visible meeting' },
        { source: 'b', sourceKind: 'Meeting', sourceNameOmitted: true }] },
    ]) assert.equal(presentation.hitTitle({ name: 'Protokoll oeffentlich', fileName: 'Minutes.pdf', evidence }), 'Minutes.pdf',
      'partial relationship metadata must not falsely establish a unique title');
    const rendered = presentation.renderHit({ id: fileId, kind: 'Page', page: 2, name: 'Original PDF',
      snippet: 'The matching document text.', dateRole: 'auto', evidence: { citation: {
        documentVersion: { id: 'urn:test:version', sha256: 'fixture-hash' } },
      relationshipPath: [{ role: 'paper.mainFile' }] } }, 1);
    const excerpt = descendants(rendered).find(node => node.tagName === 'FIGURE');
    assert.equal(excerpt.children[0].textContent, 'Textauszug aus der PDF \u00b7 Seite 2');
    assert.equal(excerpt.children[1].tagName, 'BLOCKQUOTE');
    assert.equal(excerpt.children[1].children[0].textContent, 'The matching document text.');
    assert.ok(!rendered.children.some(node => /SHA-256|Quellenrolle/.test(node.textContent || '')));
    assert.ok(rendered.children.find(node => node.tagName === 'DETAILS').children
      .some(node => /SHA-256: fixture-hash/.test(node.textContent || '')), 'technical evidence remains inside collapsed details');
    const render = hit => presentation.renderHit({ id: fileId, kind: 'Page', page: 20,
      name: 'Protokoll \u00f6ffentlich', dateRole: 'document.date', ...hit }, 3);
    const headingLink = hit => render(hit).children.find(node => node.tagName === 'H2').children[0];
    const pdfLink = headingLink({ documentUrl: 'https://ris.test/protocol.pdf' });
    assert.equal(pdfLink.href, 'https://ris.test/protocol.pdf');
    assert.equal(pdfLink.target, '_blank');
    assert.equal(pdfLink.rel, 'noopener noreferrer');
    const metadataLink = headingLink({});
    assert.equal(new URL(metadataLink.href, 'http://localhost').pathname, '/api/object');
    assert.equal(new URL(metadataLink.href, 'http://localhost').searchParams.get('page'), '20');
    assert.equal(new URL(headingLink({ documentUrl: 'data:invalid' }).href, 'http://localhost').pathname, '/api/object',
      'an unusable document URL must still leave the title clickable');
    assert.equal(new URL(headingLink({ relatedPaper: paperId, documentUrl: 'https://ris.test/protocol.pdf' }).href,
      'http://localhost').pathname, '/paper', 'a direct Paper owner keeps its local case link');
    assert.equal(new URL(headingLink({ id: objectId('meetings/1'), kind: 'Meeting', page: 0 }).href,
      'http://localhost').pathname, '/meeting');
    const dated = render({ evidence: { documentDates: {
      created: { value: '2026-07-07T07:42:51+02:00', precision: 'instant' },
      modified: { value: '2026-07-10T12:06:45+02:00', precision: 'instant' } },
    relationshipPath: [{ source: 'meeting/1', sourceKind: 'Meeting', sourceDate: {
      value: '2026-06-24T17:00:00+02:00', precision: 'instant' } }] } });
    const visibleDates = dated.children.filter(node => node.className === 'meta muted')
      .flatMap(node => node.children.map(child => child.textContent));
    assert.deepEqual(Array.from(visibleDates), ['Dokumentdatum nicht angegeben', 'Sitzung: 24.06.2026',
      'Dateieintrag erstellt: 07.07.2026', 'Dateieintrag ge\u00e4ndert: 10.07.2026']);
    const partialDates = render({ evidence: { relationshipsComplete: false, relationshipTotal: 6,
      relationsAvailable: { id: fileId, view: 'relations', direction: 'incoming', snapshotId: 'snapshot' },
      relationshipPath: [{ source: 'meeting/1', sourceKind: 'Meeting', sourceDate: {
        value: '2026-06-24', precision: 'day' } }] } });
    assert.ok(!partialDates.children.filter(node => node.className === 'meta muted')
      .flatMap(node => Array.from(node.children)).some(node => /^Sitzung:/.test(node.textContent)),
      'a truncated relationship set cannot establish a unique meeting date');
    assert.ok(partialDates.children.find(node => node.tagName === 'DETAILS').children
      .some(node => node.tagName === 'A' && node.href.includes('view=relations')));
    const ambiguous = render({ evidence: { relationshipPath: [
      { source: 'meeting/1', sourceKind: 'Meeting', sourceDate: { value: '2026-06-24', precision: 'day' } },
      { source: 'meeting/2', sourceKind: 'Meeting', sourceDate: { value: '2026-06-25', precision: 'day' } },
    ] } });
    assert.ok(!ambiguous.children.filter(node => node.className === 'meta muted')
      .flatMap(node => Array.from(node.children)).some(node => /^Sitzung:/.test(node.textContent)),
    'do not choose an arbitrary meeting date for a shared document');
    const partlyDated = render({ evidence: { relationshipPath: [
      { source: 'meeting/1', sourceKind: 'Meeting', sourceDate: { value: '2026-06-24', precision: 'day' } },
      { source: 'meeting/2', sourceKind: 'Meeting', sourceDate: null },
    ] } });
    assert.ok(!partlyDated.children.filter(node => node.className === 'meta muted')
      .flatMap(node => Array.from(node.children)).some(node => /^Sitzung:/.test(node.textContent)),
    'a second meeting with no date still makes the context ambiguous');
    const invalid = render({ evidence: { documentDates: {
      created: { value: '2026-02-30', precision: 'invalid' } } } });
    assert.ok(invalid.children.some(node => node.children.some(child =>
      child.textContent === 'Dateieintrag erstellt: Ung\u00fcltiges Datum: 2026-02-30')));
    const enriched = render({ snippet: 'Passender Protokolltext.', preparation: {
      status: 'complete', selection: 'matching-text', fields: [
        { field: 'document.date', value: '2026-07-16', status: 'rule-supported', page: 1 },
        { field: 'originator.label', value: '<img src=x onerror=alert(1)>', status: 'rule-supported', page: 1 },
        { field: 'answer.dueDate', value: '2026-08-01', status: 'rule-supported', page: 1 },
      ], structure: { agendaItems: 2, tables: 1, uncertainRows: 1 }, fieldIssues: 1,
      context: { kind: 'protocol-top', number: '3.12', status: 'rule-supported',
        heading: { text: 'Top 3.12 Erhalt einer Stra\u00dfe' },
        references: [{ text: '26/SVV/0123' }], outcome: { text: 'vertagt' } },
    } });
    const enrichedNodes = descendants(enriched);
    assert.ok(enrichedNodes.some(n => n.textContent === '16.07.2026'));
    assert.ok(enrichedNodes.some(n => n.textContent === '01.08.2026'));
    assert.ok(enrichedNodes.some(n => n.textContent === 'Ergebnis laut PDF: vertagt'));
    assert.ok(enrichedNodes.some(n => n.textContent === '<img src=x onerror=alert(1)>'));
    assert.ok(!enrichedNodes.some(n => n.tagName === 'IMG'), 'Extracted source text is never interpreted as HTML');
    assert.equal(enriched.children.find(n => n.tagName === 'H2').children[0].textContent,
      'Top 3.12 Erhalt einer Stra\u00dfe', 'A source-supported TOP title improves a generic protocol title');
    const structured = { tagName: 'DIV', children: [], append(...nodes) { this.children.push(...nodes); } };
    presentation.renderPdfPage(structured, { pageData: { page: 1, layout: { text: '\u00c4\nGremium' },
      structure: { status: 'ambiguous', agendaItems: [], diagnostics: [], tables: [{
        kind: 'consultation', columns: ['date', 'committee', 'responsibility'],
        rows: [{ status: 'ambiguous', cells: [
          { text: '', sources: [] }, { text: 'Gremium', sources: [{ start: 3, length: 7 }] },
          { text: '', sources: [] },
        ] }],
      }] } } });
    const cells = descendants(structured).filter(n => n.tagName === 'TD');
    assert.equal(cells[0].textContent, 'Nicht angegeben');
    assert.equal(descendants(structured).find(n => n.tagName === 'TR' && n.dataset.uncertain).dataset.uncertain, 'true');
    const proof = descendants(structured).find(n => n.className === 'cell-proof');
    proof.open = true; proof.listeners.toggle();
    assert.equal(descendants(proof).find(n => n.tagName === 'PRE').textContent, 'Gremium',
      'Source quotes slice UTF-8 bytes, not JavaScript UTF-16 character offsets');
    structured.children.length = 0;
    presentation.renderPdfPage(structured, { status: 'failed', error: 'invalid PDF', fields: [
      { field: 'document.date', status: 'ambiguous', page: 1, sourceLabel: 'Datum',
        sourceText: '01.02.2026 oder 02.03.2026' },
    ] });
    const failedNodes = descendants(structured);
    assert.ok(failedNodes.some(n => n.textContent === 'Mehrdeutig'), 'Unusable form fields remain inspectable');
    assert.ok(failedNodes.some(n => n.tagName === 'PRE' && n.textContent === '01.02.2026 oder 02.03.2026'));
    assert.ok(failedNodes.some(n => n.textContent === 'PDF-Textextraktion fehlgeschlagen: invalid PDF'));
    assert.ok(!failedNodes.some(n => /Neuaufbau/.test(n.textContent)), 'Extraction failure is not an outdated-index warning');

    idle = connect(port, '127.0.0.1');
    await once(idle, 'connect');
    const cases = [
      ['Schulweg', {}],
      ['NOT nonexistenttoken', { kind: 'Person', limit: '100' }],
      [Array(64).fill('Schulweg').join(' OR '), { kind: 'Page' }],
      ['GRO\u0308SSE', { kind: 'Page' }],
      ['Schulweg OR NOT Haushalt', { kind: 'Paper', year: '2024' }],
      ['"sicherer schulweg"', { kind: 'Page', year: '2023', paperType: 'Antrag', organization: 'alpha' }],
      ['Schulweg', { kind: 'Page', year: '2023', paperType: 'Antwort' }],
      ['Schulweg', { kind: 'Page', year: '2024', paperType: 'Antwort', limit: '1' }],
      ['NOT nonexistenttoken', { kind: 'Person', sort: 'date-desc', offset: '7', limit: '50' }],
      ['NOT nonexistenttoken', { kind: 'Person', sort: 'date-asc', offset: '1095' }],
      ['NOT nonexistenttoken', { kind: 'Person', offset: '4294967295' }],
      ['Schulweg', { kind: 'Page', year: '2024', sort: 'date-desc' }],
      ['Schulweg', { group: 'paper', sort: 'date-desc', limit: '1', offset: '1' }],
      ['', { kind: 'Paper', dateRole: 'paper.date', dateFrom: '2024-01-01', dateThrough: '2024-12-31', sort: 'date-asc' }],
      ['', { group: 'document', kind: 'Page', year: '2024' }],
      ['nonexistenttoken', {}],
    ];
    const flags = { kind: '--kind', year: '--year', paperType: '--paper-type',
      organization: '--organization', limit: '--limit', offset: '--offset', sort: '--sort', group: '--group',
      dateRole: '--date-role', dateFrom: '--date-from', dateThrough: '--date-through', snapshotId: '--snapshot-id' };
    for (const [q, filters] of cases) {
      if (filters.offset && filters.offset !== '0') filters.snapshotId = snapshotId;
      const response = await get('/api/search?' + new URLSearchParams({ q, ...filters }));
      assert.equal(response.status, 200);
      assert.equal(response.headers.get('cache-control'), 'no-store');
      assert.equal(await response.text(), cli([...Object.entries(filters).flatMap(([key, value]) => [flags[key], value]), q]),
        'web JSON must be byte-identical to CLI, including filters, scores/order, snippets, context and totals');
    }
    const listing = await get('/api/search?kind=Paper&dateRole=paper.date&dateFrom=2024-01-01');
    assert.equal(listing.status, 200);
    assert.equal(await listing.text(), cli(['--kind', 'Paper', '--date-role', 'paper.date', '--date-from', '2024-01-01']));
    idle.destroy(); idle = null;
    for (const args of [
      { id: fileId, page: '2' },
      { id: fileId },
    ]) {
      const response = await get('/api/object?' + new URLSearchParams(args));
      assert.equal(response.status, 200);
      assert.equal(await response.text(), cli(['--show', fileId, ...(args.page ? ['--page', args.page] : [])]));
    }
    const head = await get('/api/search?q=Schulweg', { method: 'HEAD' });
    assert.equal(head.status, 200);
    assert.equal(await head.text(), '');
    for (const [path, status] of [
      ['/api/search', 400], ['/api/search?q=', 400], ['/api/search?q=Schulweg&limit=0', 400],
      ['/api/search?q=Schulweg&year=1999', 400], ['/api/search?q=AND', 400],
      ['/api/search?q=Schulweg&q=Haushalt', 400], ['/api/search?q=%00', 400],
      ['/api/search?q=%FF', 400], ['/api/search?q=%', 400], ['/api/search?q=%A', 400],
      ['/api/search?q=Schulweg&paperType=', 400], ['/api/search?q=Schulweg&unknown=1', 400],
      ['/api/search?q=Schulweg&page=1', 400], ['/api/search?q=Schulweg&', 400],
      ['/api/search?q=Schulweg&offset=-1', 400], ['/api/search?q=Schulweg&offset=4294967296', 400],
      ['/api/search?q=Schulweg&offset=', 400], ['/api/search?q=Schulweg&offset=1&offset=2', 400],
      ['/api/search?q=Schulweg&sort=newest', 400], ['/api/search?q=Schulweg&sort=', 400],
      ['/api/search?q=Schulweg&sort=relevance&sort=date-asc', 400],
      ['/api/search?q=Schulweg&group=page', 400],
      ['/api/search?q=Schulweg&group=paper&group=record', 400],
      ['/api/search?kind=UnknownKind', 400],
      ['/api/search?kind=Meeting&dateRole=referenceYear', 400],
      ['/api/search?kind=Meeting&dateFrom=2026-02-30', 400],
      ['/api/search?kind=Meeting&dateFrom=2026-12-02&dateThrough=2026-11-04', 400],
      ['/api/search?dateRole=meeting.start&sort=date-desc', 400],
      ['/api/object?id=missing&group=paper', 400],
      ['/api/object', 400], ['/api/object?id=missing', 404], ['/api/object?id=missing&page=0', 400],
      ['/api/object?id=missing&q=foo', 400], ['/api/object?id=missing&sort=relevance', 400],
      ['/api/status?q=foo', 400],
      ['/paper', 400], ['/paper?id=missing', 404], ['/api/paper', 400],
      ['/api/paper?id=', 400], ['/api/paper?id=missing', 404],
      ['/api/paper?' + new URLSearchParams({ id: fileId }), 404],
      ['/api/paper?' + caseParameters + '&page=1', 400], ['/paper?' + caseParameters + '&q=Schulweg', 400],
      ['/api/paper?' + caseParameters + '&id=other', 400],
      ['/missing', 404], ['/%2e%2e/README.md', 404],
    ]) {
      const response = await get(path);
      assert.equal(response.status, status, path);
      assert.ok((await response.json()).error, path);
    }
    const post = await get('/api/search?q=Schulweg', { method: 'POST' });
    assert.equal(post.status, 405);
    assert.ok((await post.json()).error);
    const allowed = await get('/api/search?q=Schulweg', { headers: { Origin: base } });
    assert.equal(allowed.status, 200);
    await allowed.arrayBuffer();
    const host = `Host: 127.0.0.1:${port}\r\n`;
    for (const requestHeaders of [
      'Host: attacker.example\r\n',
      `${host}Origin: https://attacker.example\r\n`,
      `${host}Sec-Fetch-Site: same-site\r\n`,
    ]) assert.match(await raw(`GET /api/search?q=Schulweg HTTP/1.1\r\n${requestHeaders}\r\n`), /^HTTP\/1.1 403 /);
    for (const request of [
      'GET / HTTP/1.1\r\n\r\n',
      `GET / HTTP/1.1\r\n${host}${host}\r\n`,
      `GET / HTTP/1.1\r\n${host}Content-Length: 0\r\n\r\n`,
      `GET / HTTP/1.1\r\n${host}\r\nGET / HTTP/1.1\r\n${host}\r\n`,
      `GET / HTTP/1.1\r\n${host}X-Test: a\0b\r\n\r\n`,
    ]) assert.match(await raw(request), /^HTTP\/1.1 400 /);
    const oversizedPrefix = `GET / HTTP/1.1\r\n${host}X-Long: `;
    assert.match(await raw(oversizedPrefix + 'x'.repeat(65536 - oversizedPrefix.length)),
      /^HTTP\/1.1 431 /);
    await testSearchMcp(base, raw, cli, fileId, port);
    for (let i = 0; i < 8; i++) {
      const socket = connect(port, '127.0.0.1');
      held.push(socket);
      await once(socket, 'connect');
    }
    await delay(25);
    assert.match(await raw(''), /^HTTP\/1.1 503 /);
    for (const socket of held) socket.destroy();
    const timedOut = raw(`GET / HTTP/1.1\r\n${host}X-Partial: `);
    const bodyTimedOut = raw(`POST /mcp HTTP/1.1\r\n${host}Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: 100\r\n\r\n{`);
    assert.equal((await get('/api/status')).status, 200, 'partial requests must not block other clients');
    assert.match(await timedOut, /^HTTP\/1.1 408 /);
    assert.match(await bodyTimedOut, /^HTTP\/1.1 408 /);
    assert.equal((await get('/api/status')).status, 200, 'server must recover from rejected requests');
    assert.equal(child.exitCode, null, log);
    assert.equal(stdout, '', 'HTTP responses must not leak to process stdout');
    for (const args of [
      ['--port', '8080', 'Schulweg'], ['--serve', '--year', '2024'], ['--serve', '--json'],
      ['--serve', 'Schulweg'], ['--serve', '--batch'], ['--serve', '--port', '0'],
      ['--serve', '--sort', 'relevance'], ['--serve', '--offset', '0'],
    ]) {
      const invalid = spawnSync(binary, ['--index', index, ...args],
        { encoding: 'utf8', timeout: 5000, maxBuffer: 1024 * 1024 });
      assert.equal(invalid.status, 1, invalid.error ?? invalid.stderr);
    }
    console.log('Web tests passed: CLI-equivalent JSON, embedded UI, content retrieval, persistent filter changes, input errors, origin/Host checks, HEAD, connection limits and header deadlines.');
  } finally {
    if (idle) idle.destroy();
    for (const socket of held) socket.destroy();
    if (child.pid && child.exitCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGTERM');
      const timer = setTimeout(() => child.kill('SIGKILL'), 3000);
      timer.unref();
      await exited;
      clearTimeout(timer);
    }
  }
}

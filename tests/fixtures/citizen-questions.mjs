import { createHash } from 'node:crypto';
import { mkdir, utimes, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { pdf, stream } from '../pdf-fixture.mjs';

export const schema = 'https://schema.oparl.org/1.1/';
export const origin = 'https://ris.test/oparl/';
export const id = path => origin + path;
export const hash = data => createHash('sha256').update(data).digest('hex');
export const context = {
  asOf: '2026-11-06T12:00:00+01:00',
  timeZone: 'Europe/Berlin',
  window: { from: '2026-05-06', through: '2026-11-06', inclusive: true },
  calendar2026: { from: '2026-01-01', through: '2026-12-31', dateRole: 'meeting.start' },
  referenceYear: 'The YY/ prefix of Paper.reference, not the document or meeting year.',
  countingUnits: {
    paper: 'One exact OParl Paper id; not automatically one political initiative.',
    page: 'One File id and 1-based PDF page.',
    meeting: 'One exact OParl Meeting id.',
    documentDays: 'Difference of document calendar dates; NOT verified processing time.',
  },
};
export const truth = {
  attribution: {
    questions: ['A2', 'B3', 'E5'], unit: 'paper', dateRole: 'Paper.date',
    queries: ['"26/SVV/0736"', 'Schulwegsicher*'],
    papers: [id('papers/school')],
    excludedPapers: [id('papers/tax'), id('papers/tax-answer')],
    retainedPage: { id: id('files/packet'), page: 1 },
  },
  shared: {
    questions: ['B4'], unit: 'paper', dateRole: 'referenceYear',
    query: 'Radverkehr', papers: [id('papers/joint-a'), id('papers/joint-b')].sort(),
  },
  meetings: {
    questions: ['A1', 'A2', 'A3', 'A4'], unit: 'meeting', dateRole: 'meeting.start',
    query: 'Stadtverordnetenversammlung',
    descending: ['late', 'early', 'dst-late', 'dst-early', 'unknown'].map(n => id(`meetings/${n}`)),
    unknownStart: id('meetings/unknown'),
    occurred: null,
    reasonUnknown: 'Past invitation dates do not prove that a meeting took place.',
  },
  dates: {
    questions: ['E1', 'E2', 'E3'], unit: 'documentDays', dateRole: 'document.date',
    rawPaperDate: '2026-06-30', requestDocumentDate: '24.04.2026', answerDocumentDate: '18.06.2026',
    documentDeltaDays: 55, submittedAt: null, answeredAt: null, processingDays: null,
    reasonUnknown: 'Document dates are not verified receipt/answer event dates.',
  },
  roles: {
    questions: ['E1', 'E2'], unit: 'paper', dateRole: 'document.date',
    extension: { id: id('papers/extension'), rawType: 'Antwort auf Kleine Anfrage', finalAnswer: false },
    answer: { id: id('papers/answer'), rawType: 'Kleine Anfrage', substantiveAnswer: true },
    individualVotes: null, actualAuthors: null,
    reasonUnknown: 'A majority is not an individual vote list; responsibility is not authorship.',
  },
  compact: { questions: ['B2', 'E5'], unit: 'httpResponseBytes', maxBytesExclusive: 20000 },
};

function document(pages) {
  const objects = [null, '<< /Type /Catalog /Pages 2 0 R >>', null,
    '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>'];
  const children = [];
  for (const lines of pages) {
    const page = objects.length, content = page + 1;
    children.push(`${page} 0 R`);
    objects.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents ${content} 0 R >>`);
    objects.push(stream(`BT /F1 12 Tf 14 TL 10 700 Td ${lines.map(line =>
      `(${line.replace(/[\\()]/g, '\\$&')}) Tj T*`).join(' ')} ET`));
  }
  objects[2] = `<< /Type /Pages /Count ${children.length} /Kids [${children.join(' ')}] /Resources << /Font << /F1 3 0 R >> >> >>`;
  return pdf(objects);
}

export async function createCitizenFixture(root) {
  const archive = join(root, 'archive'), documents = [];
  await mkdir(join(archive, 'metadata'), { recursive: true });
  await mkdir(join(archive, 'documents'));
  await writeFile(join(archive, 'archive.json'), JSON.stringify({ complete: true, referenceYear: 2026 }));
  const objects = [];
  const add = (kind, path, values) => {
    const object = { id: id(path), type: schema + kind, ...values };
    objects.push(object);
    return object.id;
  };
  const file = (name, pages) => {
    const bytes = document(pages), fileId = add('File', `files/${name}`, {
      name: name === 'packet' ? 'Alle Anlagen oeffentlich' : `${name}.pdf`,
      fileName: `${name}.pdf`, created: '2000-01-01T00:00:00+01:00',
      accessUrl: `https://ris.test/doc/${name}`,
    });
    documents.push({ id: fileId, bytes });
    return fileId;
  };
  const school = file('school', [['26/SVV/0736', 'Fortschreibung Schulwegsicherungskonzept',
    'Beschlossen am 23.09.2026. Bericht im Dezember 2026 und im Mai 2027.',
    'Abstimmung: mit Stimmenmehrheit angenommen.']]);
  const packet = file('packet', [
    ['26/SVV/0736', 'Stellungnahme zum Schulwegsicherungskonzept.'],
    ['26/SVV/0873', 'Stellungnahme zur Verpackungssteuer.'],
  ]);
  const tax = file('tax', [['26/SVV/0873', 'Ruecknahme der Verpackungssteuer.']]);
  const shared = file('shared', [['Gemeinsamer Radverkehr braucht sichere Kreuzungen.']]);
  const request = file('request', [['26/SVV/0386 Kleine Anfrage', 'Datum 24.04.2026',
    'Vermuellung Parkplatz Voltaireweg.']]);
  const answer = file('answer', [['26/SVV/0386-002 Kleine Anfrage', 'Datum 18.06.2026',
    'Antwort der Verwaltung: Der Sachverhalt wird hiermit beantwortet.']]);
  const extension = file('extension', [['26/SVV/0386-001 Antwort auf Kleine Anfrage',
    'Datum 20.05.2026', 'Fristverlaengerungsbitte, keine Sachantwort.']]);
  const paper = (name, reference, values) => add('Paper', `papers/${name}`, {
    reference, date: '2026-09-08', paperType: 'Antrag', ...values,
  });
  const council = add('Organization', 'organizations/council', { name: 'Stadtverordnetenversammlung' });
  const alpha = add('Organization', 'organizations/alpha', { name: 'Fraktion Alpha' });
  const beta = add('Organization', 'organizations/beta', { name: 'Fraktion Beta' });
  paper('school', '26/SVV/0736', { name: 'Fortschreibung Schulwegsicherungskonzept',
    mainFile: school, consultation: [id('consultations/school')], underDirectionOf: [alpha] });
  paper('tax', '26/SVV/0873', { name: 'Ruecknahme Verpackungssteuer', date: '2026-10-01',
    mainFile: tax, consultation: [id('consultations/tax')], subordinatedPaper: [id('papers/tax-answer')] });
  paper('tax-answer', '26/SVV/0873-001', { name: 'Antwort Verpackungssteuer', date: '2026-10-30',
    paperType: 'Antwort', superordinatedPaper: [id('papers/tax')], mainFile: tax });
  paper('joint-a', '26/SVV/0100', { name: 'Gemeinsame Anlage A', mainFile: shared, underDirectionOf: [alpha] });
  paper('joint-b', '25/SVV/0101', { name: 'Gemeinsame Anlage B', paperType: 'Antwort',
    date: '2026-02-01', mainFile: shared, underDirectionOf: [beta] });
  paper('request', '26/SVV/0386', { name: 'Vermuellung Voltaireweg', date: truth.dates.rawPaperDate,
    paperType: 'Kleine Anfrage', mainFile: request, relatedPaper: [id('papers/not-indexed')],
    subordinatedPaper: [id('papers/extension'), id('papers/answer')] });
  paper('extension', '26/SVV/0386-001', { name: 'Fristverlaengerungsbitte Voltaireweg',
    paperType: truth.roles.extension.rawType, mainFile: extension, superordinatedPaper: [id('papers/request')] });
  paper('answer', '26/SVV/0386-002', { name: 'Vermuellung Voltaireweg', date: truth.dates.rawPaperDate,
    paperType: truth.roles.answer.rawType, mainFile: answer, superordinatedPaper: [id('papers/request')] });
  const agenda = [];
  for (const name of ['school', 'tax']) {
    const agendaId = add('AgendaItem', `agendaItems/${name}`, {
      name, meeting: id('meetings/late'), consultation: id(`consultations/${name}`),
    });
    agenda.push(agendaId);
    add('Consultation', `consultations/${name}`, { paper: id(`papers/${name}`),
      meeting: id('meetings/late'), agendaItem: agendaId, authoritative: true });
  }
  const unrelatedAgenda = Array.from({ length: 128 }, (_, n) => ({
    id: id(`agendaItems/unrelated-${n}`), type: schema + 'AgendaItem', number: String(n + 10),
    name: `Unabhaengiger Tagesordnungspunkt ${n}: andere Aufgaben ohne Bezug zu diesem Vorgang.`,
  }));
  const starts = {
    late: '2026-11-04T19:00:00+01:00', early: '2026-11-04T15:00:00+01:00',
    'dst-late': '2026-10-25T02:15:00+01:00', 'dst-early': '2026-10-25T02:45:00+02:00',
  };
  for (const name of ['unknown', 'early', 'dst-early', 'dst-late', 'late'])
    add('Meeting', `meetings/${name}`, {
      name: 'Stadtverordnetenversammlung', meetingState: 'eingeladen', organization: [council],
      ...(starts[name] ? { start: starts[name] } : {}),
      ...(name === 'late' ? { auxiliaryFile: [packet], agendaItem: [...agenda, ...unrelatedAgenda] } : {}),
    });
  for (const object of objects) {
    const path = join(archive, 'metadata', `${hash(object.id)}.json`);
    await writeFile(path, JSON.stringify(object));
    await utimes(path, 1793962800, 1793962800);
  }
  for (const entry of documents)
    await writeFile(join(archive, 'documents', `${hash(entry.id)}-fixture.pdf`), entry.bytes);
  return { archive, starts, objects, documents,
    digest: hash(Buffer.concat([Buffer.from(JSON.stringify(objects)), ...documents.map(d => d.bytes)])) };
}

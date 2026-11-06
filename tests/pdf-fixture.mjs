import { deflateSync } from 'node:zlib';

export function pdf(objects) {
  const parts = [Buffer.from('%PDF-1.7\n')], offsets = [0];
  let length = parts[0].length;
  for (let i = 1; i < objects.length; i++) {
    offsets[i] = length;
    const body = Buffer.isBuffer(objects[i]) ? objects[i] : Buffer.from(objects[i], 'latin1');
    const data = Buffer.concat([Buffer.from(`${i} 0 obj\n`), body, Buffer.from('\nendobj\n')]);
    parts.push(data); length += data.length;
  }
  const xref = [`xref\n0 ${objects.length}\n0000000000 65535 f \n`,
    ...offsets.slice(1).map(offset => `${String(offset).padStart(10, '0')} 00000 n \n`),
    `trailer\n<< /Size ${objects.length} /Root 1 0 R >>\nstartxref\n${length}\n%%EOF\n`];
  return Buffer.concat([...parts, Buffer.from(xref.join(''))]);
}

export function stream(text) {
  const bytes = deflateSync(Buffer.from(text, 'latin1'));
  return Buffer.concat([Buffer.from(`<< /Length ${bytes.length} /Filter /FlateDecode >>\nstream\n`),
    bytes, Buffer.from('\nendstream')]);
}

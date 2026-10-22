import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

import { validateImage, parseManifest, latestRelease, fetchRelease, sha256Hex, BOARDS } from '../js/images.js';

function img(size, sp, reset) {
  const b = new Uint8Array(size);
  const dv = new DataView(b.buffer);
  dv.setUint32(0, sp, true);
  dv.setUint32(4, reset, true);
  return b;
}

test('validateImage: limits from the M32 flash map', () => {
  assert.ok(validateImage(img(1024, 0x20005000, 0x08003101)).ok);
  assert.ok(validateImage(img(0x18800, 0x20000000, 0x0801b7ff)).ok);
  const cases = [
    [img(0x18801, 0x20005000, 0x08003101), /Too large/],
    [img(64, 0x20005004, 0x08003101), /stack pointer/],
    [img(64, 0x1fffffff, 0x08003101), /stack pointer/],
    [img(64, 0x20005000, 0x08003100), /Thumb/],
    [img(64, 0x20005000, 0x08002fff), /outside the application/],
    [img(64, 0x20005000, 0x0801b801), /outside the application/],
    [new Uint8Array(4), /too small/],
  ];
  for (const [b, re] of cases) {
    const v = validateImage(b);
    assert.equal(v.ok, false);
    assert.match(v.errors.join(' '), re);
  }
});

test('shipped manifest.json parses; placeholder release is not offered', () => {
  const json = JSON.parse(readFileSync(new URL('../firmware/manifest.json', import.meta.url)));
  const [m32] = parseManifest(json);
  assert.equal(m32.id, 'm32');
  assert.deepEqual(m32.usb, { vid: 0x17cc, appPid: 0x1860, dfuPid: 0x1862 });
  assert.equal(m32.appBase, BOARDS.m32.appBase);
  assert.equal(m32.releases[0].available, false);
  assert.equal(latestRelease(m32), null);
});

test('fetchRelease checks size and SHA-256', async () => {
  const bin = img(4096, 0x20005000, 0x08003101);
  const rel = { version: '1.0.0', file: 'firmware/m32/x.bin', size: bin.length, sha256: await sha256Hex(bin) };
  const [board] = parseManifest({ boards: [{ id: 'm32', releases: [rel] }] });
  assert.equal(latestRelease(board).version, '1.0.0');
  const fetchFn = async () => ({ ok: true, arrayBuffer: async () => bin.buffer.slice(0) });
  assert.deepEqual(await fetchRelease(rel, { fetchFn }), bin);
  await assert.rejects(fetchRelease({ ...rel, sha256: '0'.repeat(64) }, { fetchFn }), /checksum/);
  await assert.rejects(fetchRelease({ ...rel, size: 1 }, { fetchFn }), /expected 1/);
  await assert.rejects(fetchRelease(rel, { fetchFn: async () => ({ ok: false, status: 404 }) }), /404/);
});

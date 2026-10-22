import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { zipSync } from '../vendor/fflate.js';
import { STOCK_IMAGES, stockFromFile, extractExecutables, preferredType, recallType, rememberType, stockName, locateImages, chooseStock, identify, findAll, StockError } from '../js/stock.js';
import { sha256Hex, validateImage } from '../js/images.js';

const REPO = join(fileURLToPath(new URL('.', import.meta.url)), '..', '..');
const FW = process.env.MK_FW_DIR || join(REPO, 'fw'); // NI files: read in place, never copied
const STOCK_BIN = join(FW, 'm32_v140.bin');
const ZIP_140 = join(FW, 'KKM_FWUpdater_140_Mac.zip');
const ZIP_317 = join(FW, 'KKM_FWUpdater_3.17.0_Mac.zip');
const EXE = 'KKMUpdater.app/Contents/MacOS/KKMUpdater';

// Deterministic filler that never contains a stock signature by accident.
function filler(n, seed = 1) {
  const b = new Uint8Array(n);
  let x = seed;
  for (let i = 0; i < n; i++) { x = (x * 1103515245 + 12345) >>> 0; b[i] = x >>> 24; }
  return b;
}

function fakeExecutable(image, offset, total) {
  const exe = filler(total);
  // A decoy: right signature, wrong body. Must be skipped by the SHA-256 check.
  exe.set(image.subarray(0, 8), 1000);
  exe.set(image, offset);
  return exe;
}

// Synthetic "stock" table entry so the zip path is tested even without NI files.
async function fakeTable() {
  const img = filler(20000, 7);
  const dv = new DataView(img.buffer);
  dv.setUint32(0, 0x20004000, true);
  dv.setUint32(4, 0x08003201, true);
  const entry = {
    board: 'm32', label: 'fake', flashType: 'none', length: img.length,
    sha256: await sha256Hex(img), signature: [...img.subarray(0, 8)].map((b) => b.toString(16).padStart(2, '0')).join(''),
  };
  return { img, table: [entry] };
}

test('findAll finds every occurrence, including unaligned ones', () => {
  const hay = Uint8Array.of(1, 2, 3, 1, 2, 3, 9, 1, 2);
  assert.deepEqual([...findAll(hay, Uint8Array.of(1, 2, 3))], [0, 3]);
  assert.deepEqual([...findAll(hay, Uint8Array.of(1, 2, 3, 4))], []);
});

test('stock table entries are self-consistent and pass image validation rules', () => {
  for (const e of STOCK_IMAGES) {
    assert.match(e.sha256, /^[0-9a-f]{64}$/);
    assert.match(e.signature, /^[0-9a-f]{16}$/);
    assert.ok(e.length > 0 && e.length <= 0x18800);
    const sig = Uint8Array.from(e.signature.match(/../g), (h) => parseInt(h, 16));
    const v = validateImage(new Uint8Array([...sig, ...new Uint8Array(8)]));
    assert.ok(v.ok, `${e.label}: ${v.errors}`);
  }
});

test('fake updater zip: image found inside the executable at an odd offset (synthetic image)', async () => {
  const { img, table } = await fakeTable();
  const exe = fakeExecutable(img, 123457, 300000);
  const zip = zipSync({
    [EXE]: exe,
    'KKMUpdater.app/Contents/Info.plist': new TextEncoder().encode('<plist/>'),
    ['__MACOSX/' + EXE]: exe.slice(0, 4096), // resource-fork junk with the same path
  });
  assert.deepEqual(extractExecutables(zip).map((f) => f.name), [EXE]);
  const found = await stockFromFile(zip, 'm32', table);
  assert.equal(found.length, 1);
  assert.equal(found[0].offset, 123457);
  assert.equal(found[0].source, EXE);
  assert.deepEqual(found[0].bytes, img);
});

test('zip without KKMUpdater.app is rejected with a clear error', async () => {
  const zip = zipSync({ 'readme.txt': new TextEncoder().encode('hello') });
  await assert.rejects(stockFromFile(zip), (e) => e instanceof StockError && e.code === 'no-updater');
});

test('updater without a known image -> not-found', async () => {
  const zip = zipSync({ [EXE]: filler(50000) });
  await assert.rejects(stockFromFile(zip), (e) => e.code === 'not-found');
});

test('random file -> not-found; broken zip -> bad-zip', async () => {
  await assert.rejects(stockFromFile(filler(1000)), (e) => e.code === 'not-found');
  const broken = Uint8Array.of(0x50, 0x4b, 0x03, 0x04, 1, 2, 3);
  await assert.rejects(stockFromFile(broken), (e) => e.code === 'bad-zip');
});

test('chooseStock picks by flash type and defaults to "none"', () => {
  const a = { entry: { flashType: 'none' } }, b = { entry: { flashType: 'non3' } };
  assert.equal(chooseStock([b, a], null), a);
  assert.equal(chooseStock([a, b], 'non3'), b);
  assert.equal(chooseStock([a], 'non3'), null);
  assert.equal(chooseStock([], null), null);
});

const noBin = !existsSync(STOCK_BIN) && 'fw/m32_v140.bin not present';

test('real NI image embedded in a fake updater zip', { skip: noBin }, async () => {
  const img = new Uint8Array(readFileSync(STOCK_BIN));
  const zip = zipSync({ [EXE]: fakeExecutable(img, 2_249_073, 3_000_000) });
  const found = await stockFromFile(zip);
  assert.equal(found.length, 1);
  assert.equal(found[0].entry.flashType, 'none');
  assert.equal(found[0].offset, 2_249_073);
  assert.equal(await sha256Hex(found[0].bytes), STOCK_IMAGES[0].sha256);
});

test('raw NI .bin is recognised directly', { skip: noBin }, async () => {
  const img = new Uint8Array(readFileSync(STOCK_BIN));
  assert.equal((await identify(img)).flashType, 'none');
  const found = await stockFromFile(img);
  assert.deepEqual([found.length, found[0].source], [1, 'bin']);
  assert.ok(validateImage(img).ok);
  // A modified copy is not "stock".
  const bad = img.slice(); bad[100] ^= 1;
  assert.equal(await identify(bad), null);
});

test('real KKM_FWUpdater_140_Mac.zip', { skip: !existsSync(ZIP_140) && 'fw/KKM_FWUpdater_140_Mac.zip not present' }, async () => {
  const found = await stockFromFile(new Uint8Array(readFileSync(ZIP_140)));
  assert.deepEqual(found.map((f) => f.entry.flashType), ['none']);
  assert.equal(found[0].bytes.length, 51148);
  assert.equal(chooseStock(found, 'none'), found[0]);
});

test('real KKM_FWUpdater_3.17.0_Mac.zip (both images)', { skip: !existsSync(ZIP_317) && 'fw/KKM_FWUpdater_3.17.0_Mac.zip not present' }, async () => {
  const found = await stockFromFile(new Uint8Array(readFileSync(ZIP_317)));
  assert.deepEqual(found.map((f) => f.entry.flashType).sort(), ['non3', 'none']);
  for (const f of found) assert.ok(validateImage(f.bytes).ok);
});

test('locateImages stops at the first verified copy (fat binaries hold two)', async () => {
  const { img, table } = await fakeTable();
  const exe = filler(100000);
  exe.set(img, 10); exe.set(img, 50000);
  const found = await locateImages(exe, 'm32', table);
  assert.deepEqual(found.map((f) => f.offset), [10]);
});

test('preferredType: live non-"none" wins, then remembered, then "none"', () => {
  assert.equal(preferredType({ live: 'non3', remembered: 'none' }), 'non3');
  assert.equal(preferredType({ live: 'none', remembered: 'non3' }), 'non3'); // My Kontrol answers "none"
  assert.equal(preferredType({ live: null, remembered: 'non3' }), 'non3');   // DFU mode
  assert.equal(preferredType({ live: null, remembered: null }), 'none');
  assert.equal(preferredType(), 'none');
});

test('remember/recall type per serial; "none" never overwrites; storage errors ignored', () => {
  const m = new Map();
  const storage = { getItem: (k) => m.get(k) ?? null, setItem: (k, v) => m.set(k, v) };
  assert.equal(rememberType(storage, 'S1', 'non3'), true);
  assert.equal(rememberType(storage, 'S1', 'none'), false);
  assert.equal(recallType(storage, 'S1'), 'non3');
  assert.equal(recallType(storage, 'S2'), null);
  assert.equal(rememberType(storage, '', 'non3'), false);
  const broken = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); } };
  assert.equal(rememberType(broken, 'S1', 'non3'), false);
  assert.equal(recallType(broken, 'S1'), null);
  assert.equal(recallType(null, 'S1'), null);
  assert.equal(stockName(STOCK_IMAGES[0]), 'NI firmware 1.4.0 / type none');
});

test('0xB3 strings are compiled into the NI images at the documented offsets', { skip: !existsSync(ZIP_317) && 'fw/KKM_FWUpdater_3.17.0_Mac.zip not present' }, async () => {
  const found = await stockFromFile(new Uint8Array(readFileSync(ZIP_317)));
  const at = { none: 0xc753, non3: 0xe5dc };
  for (const f of found) {
    const t = f.entry.flashType;
    assert.equal(new TextDecoder().decode(f.bytes.subarray(at[t], at[t] + 4)), t);
  }
});

import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync, writeFileSync, readFileSync, rmSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

import * as dfu from '../js/dfu.js';
import { Sim, MockDevice, MockUsb, recordingSleep, DESC } from './mockusb.js';

const HERE = fileURLToPath(new URL('.', import.meta.url));
const REPO = join(HERE, '..', '..');
const M32DFU = process.env.MK_M32DFU || join(REPO, 'tools', 'm32dfu.py');
const STOCK_BIN = join(process.env.MK_FW_DIR || join(REPO, 'fw'), 'm32_v140.bin');
const HARNESS = join(HERE, 'm32dfu_trace.py');

const sha = (b) => createHash('sha256').update(b).digest('hex');

function syntheticImage(n) {
  const b = new Uint8Array(n);
  for (let i = 0; i < n; i++) b[i] = (i * 31 + 7) & 0xff;
  new DataView(b.buffer).setUint32(0, 0x20005000, true);
  new DataView(b.buffer).setUint32(4, 0x08003101, true);
  return b;
}

const python = (() => {
  const r = spawnSync('python3', ['-c', 'import sys; print(sys.version_info[0])'], { encoding: 'utf8' });
  return r.status === 0 && r.stdout.trim() === '3';
})();
const canCompare = python && existsSync(M32DFU);

function runPython(scenario, image) {
  const dir = mkdtempSync(join(tmpdir(), 'mk-dfu-'));
  try {
    const imgPath = join(dir, 'image.bin');
    if (image) writeFileSync(imgPath, image);
    const scenPath = join(dir, 'scenario.json');
    writeFileSync(scenPath, JSON.stringify({ ...scenario, image: imgPath }));
    const r = spawnSync('python3', ['-I', HARNESS, M32DFU, scenPath], { encoding: 'utf8' });
    assert.equal(r.status, 0, r.stderr);
    return JSON.parse(r.stdout);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

// Run the JS side on the same simulator; returns its trace.
async function runJsFlash(scenario, image) {
  const sim = new Sim(scenario);
  dfu.hooks.onTransfer = ({ timeout }) => { sim.nextTimeout = timeout; };
  const dev = new MockDevice(sim, 0x1862);
  let error = null;
  try {
    await dfu.flash(dev, image, { sleep: recordingSleep(sim) });
  } catch (e) {
    error = e;
  } finally {
    dfu.hooks.onTransfer = null;
  }
  return { trace: sim.trace, error, sim };
}

async function runJsEnter(scenario) {
  const sim = new Sim({ ...scenario, mode: 'app' });
  dfu.hooks.onTransfer = ({ timeout }) => { sim.nextTimeout = timeout; };
  const usb = new MockUsb(sim);
  const [app] = await usb.getDevices();
  const sleep = recordingSleep(sim);
  try {
    const r = await dfu.enterDfu(app);
    const found = await dfu.waitForDevice(usb, (d) => d.productId === 0x1862, { sleep });
    return { trace: sim.trace, r, found };
  } finally {
    dfu.hooks.onTransfer = null;
  }
}

// Python trace ends with dispose; sleep(5); describe() of whatever enumerated.
const upToDispose = (t) => t.slice(0, t.findIndex((e) => e[0] === 'dispose') + 1);

test('parseDfuInfo reads the DFU functional descriptor like m32dfu.py', () => {
  assert.deepEqual(dfu.parseDfuInfo(DESC.app), { iface: 2, proto: 1, attrs: 0x09, timeout: 255, xfer: 64 });
  assert.deepEqual(dfu.parseDfuInfo(DESC.dfu), { iface: 0, proto: 2, attrs: 0x0b, timeout: 255, xfer: 1024 });
  assert.equal(dfu.parseDfuInfo(Uint8Array.of(9, 2, 9, 0, 0, 1, 0, 0x80, 50)), null);
  assert.equal(dfu.parseDfuInfo(Uint8Array.of(0, 0, 0)), null); // zero length: no infinite loop
});

test('flash: exact transfer sequence for a 2-block image (hardcoded expectation)', async () => {
  const img = syntheticImage(1500);
  const { trace, error } = await runJsFlash({ busy_every: 1, busy_polls: 1, busy_poll_ms: 3, manifest: [[7, 10], [8, 0]] }, img);
  assert.equal(error, null);
  const GETSTATUS = ['ctrl', 0xa1, 3, 0, 0, 6, null, 5000];
  assert.deepEqual(trace, [
    ['ctrl', 0x80, 6, 0x0200, 0, 1024, null, 1000],
    GETSTATUS,
    ['ctrl', 0x21, 1, 0, 0, 1024, sha(img.subarray(0, 1024)), 5000],
    GETSTATUS, ['sleep', 3], GETSTATUS,
    ['ctrl', 0x21, 1, 1, 0, 476, sha(img.subarray(1024)), 5000],
    GETSTATUS, ['sleep', 3], GETSTATUS,
    ['ctrl', 0x21, 1, 2, 0, 0, null, 5000],
    GETSTATUS,                 // dfuMANIFEST-SYNC ends the download loop
    GETSTATUS, ['sleep', 50],  // dfuMANIFEST, poll 10 -> max(poll, 50)
    GETSTATUS,                 // dfuMANIFEST-WAIT-RESET
    ['dispose'],
  ]);
});

test('flash: never sends the 0xB3 flash-type request in DFU mode', async () => {
  const { trace } = await runJsFlash({ busy_every: 3 }, syntheticImage(4096));
  assert.ok(!trace.some((e) => e[0] === 'ctrl' && e[2] === 0xb3));
});

const FLASH_SCENARIOS = [
  ['busy polls, manifest -> wait-reset', { busy_every: 2, busy_polls: 2, busy_poll_ms: 3, manifest: [[7, 10], [8, 0]] }, 5000],
  ['dfuERROR at start -> CLRSTATUS; poll 0 -> sleep 1; manifest -> dfuIDLE -> DETACH',
    { initial_state: 10, busy_every: 1, busy_poll_ms: 0, manifest: [[7, 120], [7, 0], [2, 0]] }, 3000],
  ['exact multiple of the transfer size', { manifest: [[0, 0]] }, 3072],
  ['device leaves the bus during manifest', { busy_every: 4, manifest: [[7, 5], ['leave', 0]] }, 2100],
  ['bootloader reports errWRITE at block 2', { error_at_block: 2 }, 4000],
];

for (const [name, scen, size] of FLASH_SCENARIOS) {
  test(`flash matches m32dfu.py: ${name}`, { skip: !canCompare && 'python3 or tools/m32dfu.py not available' }, async () => {
    const img = syntheticImage(size);
    const py = runPython({ cmd: 'flash', mode: 'dfu', ...scen }, img);
    const js = await runJsFlash(scen, img);
    if (py.at(-1)[0] === 'exit') {
      // m32dfu.py raises SystemExit; flash() throws with the same block.
      assert.deepEqual(js.trace, py.slice(0, -1));
      assert.equal(js.error.code, 'status');
      assert.match(py.at(-1)[1], new RegExp(`block ${js.error.block}:`));
    } else {
      assert.equal(js.error, null);
      assert.deepEqual(js.trace, upToDispose(py));
    }
  });
}

test('flash matches m32dfu.py with the real NI image (fw/m32_v140.bin)',
  { skip: (!canCompare && 'python3 or tools/m32dfu.py not available') || (!existsSync(STOCK_BIN) && 'fw/m32_v140.bin not present') },
  async () => {
    const img = new Uint8Array(readFileSync(STOCK_BIN));
    const scen = { busy_every: 7, busy_polls: 3, busy_poll_ms: 20, manifest: [[7, 30], [8, 0]] };
    const py = runPython({ cmd: 'flash', mode: 'dfu', ...scen }, img);
    const js = await runJsFlash(scen, img);
    assert.equal(js.error, null);
    assert.deepEqual(js.trace, upToDispose(py));
    assert.equal(js.trace.filter((e) => e[0] === 'ctrl' && e[2] === 1).length, 51); // 50 blocks + zero-length
  });

for (const detachRaises of [false, true]) {
  test(`enter matches m32dfu.py (detach ${detachRaises ? 'drops off the bus' : 'returns'})`,
    { skip: !canCompare && 'python3 or tools/m32dfu.py not available' }, async () => {
      const py = runPython({ cmd: 'enter', mode: 'app', detach_raises: detachRaises });
      const js = await runJsEnter({ detach_raises: detachRaises });
      assert.equal(js.r.alreadyDfu, false);
      assert.equal(js.r.flashType, 'none');
      assert.equal(js.found.productId, 0x1862);
      // JS: descriptor, 0xB3, DETACH(wValue 255, wIndex 2), release, sleep 500, found.
      assert.deepEqual(js.trace, [
        ['ctrl', 0x80, 6, 0x0200, 0, 1024, null, 1000],
        ['ctrl', 0xc0, 0xb3, 0, 1, 4, null, 1000],
        ['ctrl', 0x21, 0, 255, 2, 0, null, 5000],
        ['dispose'],
        ['sleep', 500],
      ]);
      assert.deepEqual(js.trace, py.slice(0, js.trace.length));
      // m32dfu.py then only prints diagnostics of the DFU device (descriptor x2, GETSTATUS,
      // and 0xB3, which the bootloader stalls). flash() starts with its own descriptor + GETSTATUS.
      assert.deepEqual(py.slice(js.trace.length).map((e) => [e[1], e[2]]), [[0x80, 6], [0x80, 6], [0xa1, 3], [0xc0, 0xb3]]);
    });
}

test('enter on a keyboard already in DFU mode does nothing', async () => {
  const sim = new Sim({ mode: 'dfu' });
  const r = await dfu.enterDfu(new MockDevice(sim, 0x1862));
  assert.equal(r.alreadyDfu, true);
  assert.equal(sim.trace.length, 1); // descriptor read only
});

test('probe reads flash type in app mode only', async () => {
  const app = await dfu.probe(new MockDevice(new Sim({ mode: 'app' }), 0x1860));
  assert.deepEqual([app.dfuMode, app.flashType, app.info.iface], [false, 'none', 2]);
  const boot = await dfu.probe(new MockDevice(new Sim({ mode: 'dfu' }), 0x1862));
  assert.deepEqual([boot.dfuMode, boot.flashType, boot.info.xfer], [true, null, 1024]);
});

test('errors: unplug mid-flash -> disconnected', async () => {
  const sim = new Sim({});
  const dev = new MockDevice(sim, 0x1862);
  sim.hook = (_s, bm, req, value) => { if (bm === 0x21 && req === 1 && value === 3) dev.unplugged = true; };
  const progress = [];
  await assert.rejects(dfu.flash(dev, syntheticImage(8000), { sleep: async () => {}, onProgress: (p) => progress.push(p.block) }),
    (e) => e.code === 'disconnected');
  assert.equal(Math.max(...progress), 3);
});

test('errors: stall (bootloader stalls 0xB3)', async () => {
  const dev = new MockDevice(new Sim({ mode: 'dfu' }), 0x1862);
  await dev.open();
  await assert.rejects(dfu.readFlashType(dev), (e) => e.code === 'stall');
});

test('errors: no answer -> timeout', async () => {
  const dev = new MockDevice(new Sim({ mode: 'app' }), 0x1860);
  await dfu.openDfu(dev);
  dev.hang = true;
  await assert.rejects(dfu.readFlashType(dev), (e) => e.code === 'timeout');
});

test('errors: open fails because another app holds the device -> busy', async () => {
  const dev = new MockDevice(new Sim({ mode: 'app' }), 0x1860);
  dev.open = async () => { throw new DOMException('Access denied.', 'SecurityError'); };
  await assert.rejects(dfu.enterDfu(dev), (e) => e.code === 'busy');
  const dev2 = new MockDevice(new Sim({ mode: 'app' }), 0x1860);
  dev2.claimInterface = async () => { throw new DOMException('Unable to claim interface.', 'NetworkError'); };
  await assert.rejects(dfu.enterDfu(dev2), (e) => e.code === 'busy');
});

test('errors: flashing an app-mode device is refused', async () => {
  await assert.rejects(dfu.flash(new MockDevice(new Sim({ mode: 'app' }), 0x1860), syntheticImage(100)),
    (e) => e.code === 'not-dfu');
});

test('waitForDevice gives up after 20 x 500 ms when the bootloader is not visible', async () => {
  const sleeps = [];
  const usb = { getDevices: async () => [] };
  const d = await dfu.waitForDevice(usb, () => true, { sleep: async (ms) => sleeps.push(ms) });
  assert.equal(d, null);
  assert.deepEqual(sleeps, Array(20).fill(500));
});

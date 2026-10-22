// WebUSB DFU 1.1 for NI Komplete Kontrol keyboards. No DOM: runs in the browser
// and in Node tests against a mock USBDevice.
//
// Mirrors tools/mkflash.py (which mirrors NI KKMUpdater 3.17.0) request for request:
//   enter: GET_DESCRIPTOR(config) -> vendor 0xB3 "flash type" -> DFU_DETACH
//   flash: GET_DESCRIPTOR(config) -> GETSTATUS [-> CLRSTATUS] -> DNLOAD n / GETSTATUS poll ...
//          -> zero-length DNLOAD -> GETSTATUS manifest poll [-> DETACH]
// Plain DFU 1.1: no DfuSe address command. NI's bootloader places block 0 at 0x08003000.

export const REQ = { DETACH: 0, DNLOAD: 1, UPLOAD: 2, GETSTATUS: 3, CLRSTATUS: 4, GETSTATE: 5, ABORT: 6 };
export const FLASH_TYPE_REQUEST = 0xb3;

export const STATES = {
  0: 'appIDLE', 1: 'appDETACH', 2: 'dfuIDLE', 3: 'dfuDNLOAD-SYNC', 4: 'dfuDNBUSY', 5: 'dfuDNLOAD-IDLE',
  6: 'dfuMANIFEST-SYNC', 7: 'dfuMANIFEST', 8: 'dfuMANIFEST-WAIT-RESET', 9: 'dfuUPLOAD-IDLE', 10: 'dfuERROR',
};

// pyusb's default control transfer timeout; mkflash.py relies on it for GET_DESCRIPTOR and CLRSTATUS.
const DEFAULT_TIMEOUT = 1000;

export class DfuError extends Error {
  // code: busy | permission | stall | timeout | disconnected | status | protocol | not-dfu
  constructor(code, message, extra = {}) {
    super(message);
    this.name = 'DfuError';
    this.code = code;
    Object.assign(this, extra);
  }
}

// Test hook: called with { timeout } right before each control transfer.
export const hooks = { onTransfer: null };

export const realSleep = (ms) => new Promise((r) => setTimeout(r, ms));

const TYPES = ['standard', 'class', 'vendor'];
const RECIPIENTS = ['device', 'interface', 'endpoint', 'other'];

function setupFor(bmRequestType, bRequest, wValue, wIndex) {
  return {
    requestType: TYPES[(bmRequestType >> 5) & 3],
    recipient: RECIPIENTS[bmRequestType & 0x1f],
    request: bRequest,
    value: wValue,
    index: wIndex,
  };
}

// Map a WebUSB exception to a DfuError. `stage` says what we were doing.
export function wrapUsbError(e, stage) {
  if (e instanceof DfuError) return e;
  const name = e && e.name;
  const msg = (e && e.message) || String(e);
  if (name === 'NotFoundError' || /disconnect|no device|not found/i.test(msg)) {
    return new DfuError('disconnected', `Keyboard disconnected (${stage})`, { cause: e });
  }
  // Chrome reports "Access denied" (SecurityError) or "Unable to open/claim" (NetworkError)
  // when another driver or app holds the device, or on Linux when udev denies access.
  if (stage === 'open' || stage === 'claim' || stage === 'configure') {
    return new DfuError('busy', `Could not ${stage} the keyboard: ${msg}`, { cause: e, usbName: name });
  }
  if (name === 'NetworkError') return new DfuError('disconnected', `USB transfer failed (${stage}): ${msg}`, { cause: e });
  return new DfuError('protocol', `USB error (${stage}): ${msg}`, { cause: e });
}

function withTimeout(promise, ms, stage) {
  let t;
  const timer = new Promise((_, rej) => {
    t = setTimeout(() => rej(new DfuError('timeout', `Keyboard did not answer within ${ms} ms (${stage})`)), ms);
  });
  return Promise.race([promise, timer]).finally(() => clearTimeout(t));
}

export async function ctrlIn(device, bm, req, value, index, length, timeout = DEFAULT_TIMEOUT, stage = 'control in') {
  let r;
  if (hooks.onTransfer) hooks.onTransfer({ timeout });
  try {
    r = await withTimeout(device.controlTransferIn(setupFor(bm, req, value, index), length), timeout, stage);
  } catch (e) {
    throw wrapUsbError(e, stage);
  }
  if (r.status === 'stall') throw new DfuError('stall', `Keyboard rejected the request (${stage})`);
  if (r.status !== 'ok') throw new DfuError('protocol', `Transfer ${r.status} (${stage})`);
  const dv = r.data;
  return new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength);
}

export async function ctrlOut(device, bm, req, value, index, data, timeout = DEFAULT_TIMEOUT, stage = 'control out') {
  let r;
  if (hooks.onTransfer) hooks.onTransfer({ timeout });
  try {
    const setup = setupFor(bm, req, value, index);
    const p = data && data.length ? device.controlTransferOut(setup, data) : device.controlTransferOut(setup);
    r = await withTimeout(p, timeout, stage);
  } catch (e) {
    throw wrapUsbError(e, stage);
  }
  if (r.status === 'stall') throw new DfuError('stall', `Keyboard rejected the request (${stage})`);
  if (r.status !== 'ok') throw new DfuError('protocol', `Transfer ${r.status} (${stage})`);
  return r;
}

// Same walk as mkflash.py dfu_info(): last interface with class 0xFE / subclass 1 that
// is followed by a DFU functional descriptor (type 0x21).
export function parseDfuInfo(raw) {
  let i = 0, cur = null, info = null;
  while (i + 1 < raw.length) {
    const l = raw[i], t = raw[i + 1];
    if (l === 0) break;
    if (t === 4) cur = raw.slice(i + 2, i + 8); // ifnum, alt, neps, class, subclass, proto
    if (t === 0x21 && l >= 7 && cur && cur[3] === 0xfe && cur[4] === 1) {
      info = {
        iface: cur[0], proto: cur[5], attrs: raw[i + 2],
        timeout: raw[i + 3] | (raw[i + 4] << 8), xfer: raw[i + 5] | (raw[i + 6] << 8),
      };
    }
    i += l;
  }
  return info;
}

export async function readDfuInfo(device) {
  const raw = await ctrlIn(device, 0x80, 6, 0x0200, 0, 1024, DEFAULT_TIMEOUT, 'read descriptors');
  const info = parseDfuInfo(raw);
  if (!info) throw new DfuError('protocol', 'This device has no DFU interface');
  return info;
}

// Open + configure + DFU info + claim the DFU interface (pyusb claims implicitly).
export async function openDfu(device) {
  try { if (!device.opened) await device.open(); } catch (e) { throw wrapUsbError(e, 'open'); }
  try { if (!device.configuration) await device.selectConfiguration(1); } catch (e) { throw wrapUsbError(e, 'configure'); }
  const info = await readDfuInfo(device);
  try { await device.claimInterface(info.iface); } catch (e) { throw wrapUsbError(e, 'claim'); }
  return info;
}

export async function closeQuietly(device, iface) {
  try { if (iface != null) await device.releaseInterface(iface); } catch { /* gone */ }
  try { if (device.opened) await device.close(); } catch { /* gone */ }
}

export async function getStatus(device, iface) {
  const s = await ctrlIn(device, 0xa1, REQ.GETSTATUS, 0, iface, 6, 5000, 'get status');
  if (s.length < 6) throw new DfuError('protocol', `Short DFU status (${s.length} bytes)`);
  return { status: s[0], poll: s[1] | (s[2] << 8) | (s[3] << 16), state: s[4] };
}

export async function readFlashType(device) {
  return ctrlIn(device, 0xc0, FLASH_TYPE_REQUEST, 0, 1, 4, 1000, 'read flash type');
}

export function decodeFlashType(bytes) {
  if (!bytes) return null;
  const printable = bytes.length > 0 && [...bytes].every((b) => b >= 0x20 && b < 0x7f);
  return printable ? String.fromCharCode(...bytes) : [...bytes].map((b) => b.toString(16).padStart(2, '0')).join(' ');
}

// Check a freshly selected device before the user picks an image: opens it, finds the
// DFU interface and, in app mode, reads the flash type. Leaves the device open.
export async function probe(device) {
  const info = await openDfu(device);
  const flashType = info.proto === 2 ? null : decodeFlashType(await readFlashType(device));
  return { info, flashType, dfuMode: info.proto === 2 };
}

// App mode -> NI bootloader. Returns { alreadyDfu, info, flashType }.
export async function enterDfu(device, { log = () => {} } = {}) {
  const info = await openDfu(device);
  if (info.proto === 2) {
    log('already in DFU mode');
    return { alreadyDfu: true, info, flashType: null };
  }
  // mkflash.py reads it before detaching and would abort on a stall; so do we.
  const ft = await readFlashType(device);
  const flashType = decodeFlashType(ft);
  log(`flash type: ${flashType}`);
  log(`sending DFU_DETACH wValue=${info.timeout} wIndex=${info.iface}`);
  try {
    await ctrlOut(device, 0x21, REQ.DETACH, info.timeout, info.iface, null, 5000, 'detach');
  } catch (e) {
    log(`detach returned ${e.message} (device may have dropped off the bus, expected)`);
  }
  await closeQuietly(device, info.iface);
  return { alreadyDfu: false, info, flashType };
}

// Wait for the bootloader to enumerate. mkflash.py: 20 x (sleep 0.5 s, look).
// Only finds devices this origin already has permission for; otherwise returns null
// and the caller asks the user to click Continue (requestDevice needs a gesture).
export async function waitForDevice(usb, match, { sleep = realSleep, tries = 20, intervalMs = 500, signal } = {}) {
  for (let n = 0; n < tries; n++) {
    await sleep(intervalMs);
    if (signal && signal.aborted) return null;
    const devs = await usb.getDevices();
    const d = devs.find(match);
    if (d) return d;
  }
  return null;
}

// Download `image` (Uint8Array) to a device in DFU mode.
// onProgress({ block, blocks, bytes, total, phase }) after each block.
export async function flash(device, image, { onProgress = () => {}, log = () => {}, sleep = realSleep } = {}) {
  const info = await openDfu(device);
  if (info.proto !== 2) throw new DfuError('not-dfu', 'Device is not in DFU (update) mode');
  const iface = info.iface;
  // The bootloader stalls 0xB3, so it is never sent here.
  let { state } = await getStatus(device, iface);
  if (state === 10) await ctrlOut(device, 0x21, REQ.CLRSTATUS, 0, iface, null, DEFAULT_TIMEOUT, 'clear status');

  const xfer = info.xfer || 1024;
  const blocks = [];
  for (let i = 0; i < image.length; i += xfer) blocks.push(image.subarray(i, i + xfer));
  log(`writing ${image.length} bytes in ${blocks.length} blocks of ${xfer}`);
  onProgress({ block: 0, blocks: blocks.length, bytes: 0, total: image.length, phase: 'download' });

  let written = 0;
  for (let n = 0; n <= blocks.length; n++) {
    const blk = n < blocks.length ? blocks[n] : null;
    await ctrlOut(device, 0x21, REQ.DNLOAD, n, iface, blk, 5000, blk ? `write block ${n}` : 'finish download');
    // mkflash.py polls without limit; cap it so a wedged bootloader becomes an error, not a hang.
    for (let polls = 0; ; polls++) {
      if (polls >= 1000) throw new DfuError('timeout', `Block ${n}: bootloader stayed busy`, { block: n });
      const s = await getStatus(device, iface);
      state = s.state;
      if (s.status !== 0) {
        throw new DfuError('status', `Block ${n}: status ${s.status}, state ${STATES[s.state] || s.state}`,
          { dfuStatus: s.status, dfuState: s.state, block: n });
      }
      if (state === 5 || state === 2 || (!blk && (state === 6 || state === 7 || state === 8))) break;
      await sleep(Math.max(s.poll, 1));
    }
    if (blk) {
      written += blk.length;
      onProgress({ block: n + 1, blocks: blocks.length, bytes: written, total: image.length, phase: 'download' });
    }
  }

  log(`manifest phase; state ${STATES[state]}`);
  onProgress({ block: blocks.length, blocks: blocks.length, bytes: written, total: image.length, phase: 'manifest' });
  for (let k = 0; k < 20; k++) {
    try {
      const s = await getStatus(device, iface);
      state = s.state;
      log(`status ${s.status} ${STATES[state]}`);
      if (state === 2 || state === 8 || state === 0) break;
      await sleep(Math.max(s.poll, 50));
    } catch (e) {
      log(`device left bus (expected after manifest): ${e.message}`);
      break;
    }
  }
  if (state === 2) {
    log('sending DFU_DETACH to return to app');
    try {
      await ctrlOut(device, 0x21, REQ.DETACH, info.timeout, iface, null, 5000, 'detach');
    } catch (e) {
      log(String(e.message));
    }
  }
  await closeQuietly(device, iface);
  return { state, blocks: blocks.length, xfer };
}

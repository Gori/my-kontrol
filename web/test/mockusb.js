// Simulated keyboard behind a WebUSB-shaped API. Sim must stay identical to Sim in
// m32dfu_trace.py so both tools can be compared transfer by transfer.

import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';

const raw = JSON.parse(readFileSync(new URL('./fixtures/descriptors.json', import.meta.url)));
export const DESC = { app: Uint8Array.from(raw.app.flat()), dfu: Uint8Array.from(raw.dfu.flat()) };

const TYPES = ['standard', 'class', 'vendor'];
const RECIPIENTS = ['device', 'interface', 'endpoint', 'other'];
const sha = (b) => createHash('sha256').update(b).digest('hex');
const st = (status, poll, state) => Uint8Array.of(status, poll & 0xff, (poll >> 8) & 0xff, (poll >> 16) & 0xff, state, 0);

export class UsbError extends Error {}
class Stall {}

export class Sim {
  constructor(o = {}) {
    this.mode = o.mode || 'dfu';
    this.state = o.initial_state ?? 2;
    this.busy_every = o.busy_every || 0;
    this.busy_polls = o.busy_polls ?? 1;
    this.busy_poll_ms = o.busy_poll_ms ?? 3;
    this.manifest = (o.manifest || [[8, 0]]).map((x) => [...x]);
    this.error_at_block = o.error_at_block ?? -1;
    this.detach_raises = !!o.detach_raises;
    this.pending = 0;
    this.block = -1;
    this.gone = false;
    this.trace = [];
    this.nextTimeout = null;
  }

  // Returns Uint8Array (IN), number (OUT), or throws UsbError / Stall.
  ctrl(bm, req, value, index, dataOrLen) {
    const timeout = this.nextTimeout;
    this.nextTimeout = null;
    let data = null;
    if (bm & 0x80) {
      this.trace.push(['ctrl', bm, req, value, index, dataOrLen, null, timeout]);
    } else {
      data = dataOrLen ? Uint8Array.from(dataOrLen) : new Uint8Array(0);
      this.trace.push(['ctrl', bm, req, value, index, data.length, data.length ? sha(data) : null, timeout]);
    }
    if (this.hook) this.hook(this, bm, req, value);
    if (this.gone) throw new UsbError('No such device');
    if (bm === 0x80 && req === 6) return DESC[this.mode].slice(0, dataOrLen);
    if (bm === 0xc0 && req === 0xb3) {
      if (this.mode === 'app') return new TextEncoder().encode('none');
      throw new Stall();
    }
    if (bm === 0x21 && req === 0) {
      if (this.mode === 'app') {
        this.mode = 'dfu';
        this.state = 2;
        if (this.onDetach) this.onDetach();
        if (this.detach_raises) throw new UsbError('No such device (it may have been disconnected)');
      }
      return 0;
    }
    if (bm === 0x21 && req === 4) { this.state = 2; return 0; }
    if (bm === 0x21 && req === 1) {
      if (data.length) {
        this.block = value;
        this.state = 3;
        this.pending = this.busy_every && value % this.busy_every === 0 ? this.busy_polls : 0;
      } else {
        this.state = 6;
      }
      return data.length;
    }
    if (bm === 0xa1 && req === 3) {
      if (this.state === 3) {
        if (this.error_at_block === this.block) { this.state = 10; return st(3, 0, 10); }
        if (this.pending > 0) { this.pending--; return st(0, this.busy_poll_ms, 4); }
        this.state = 5;
        return st(0, 0, 5);
      }
      if (this.state === 6) { this.state = 'M'; return st(0, 0, 6); }
      if (this.state === 'M') {
        const item = this.manifest.length > 1 ? this.manifest.shift() : this.manifest[0];
        if (item[0] === 'leave') { this.gone = true; throw new UsbError('No such device'); }
        return st(0, item[1], item[0]);
      }
      if (this.state === 10) return st(0x0e, 0, 10);
      return st(0, 0, this.state);
    }
    throw new Stall();
  }
}

// WebUSB USBDevice over a Sim.
export class MockDevice {
  constructor(sim, productId) {
    this.sim = sim;
    this.vendorId = 0x17cc;
    this.productId = productId;
    this.productName = productId === 0x1862 ? 'KOMPLETE KONTROL M32 DFU' : 'M32 (Oskar\'s Firmware)';
    this.deviceVersionMajor = 2; this.deviceVersionMinor = 0; this.deviceVersionSubminor = 0;
    this.opened = false;
    this.configuration = null;
    this.claimed = new Set();
    this.unplugged = false;
  }
  async open() { if (this.unplugged) throw new DOMException('Device unavailable.', 'NotFoundError'); this.opened = true; }
  async close() { this.sim.trace.push(['dispose']); this.opened = false; }
  async selectConfiguration(v) { this.configuration = { configurationValue: v }; }
  async claimInterface(n) { this.claimed.add(n); }
  async releaseInterface(n) { this.claimed.delete(n); }

  _bm(setup, dirIn) {
    if (!this.opened) throw new DOMException('The device must be opened first.', 'InvalidStateError');
    const t = TYPES.indexOf(setup.requestType), r = RECIPIENTS.indexOf(setup.recipient);
    if (t < 0 || r < 0) throw new TypeError('bad setup');
    if (setup.recipient === 'interface' && !this.claimed.has(setup.index & 0xff)) {
      throw new DOMException('The specified interface has not been claimed.', 'InvalidStateError');
    }
    return (dirIn ? 0x80 : 0) | (t << 5) | r;
  }
  _run(fn) {
    if (this.unplugged) return Promise.reject(new DOMException('The device was disconnected.', 'NotFoundError'));
    if (this.hang) return new Promise(() => {});
    try {
      return Promise.resolve(fn());
    } catch (e) {
      if (e instanceof Stall) return Promise.resolve({ status: 'stall' });
      if (e instanceof UsbError) return Promise.reject(new DOMException('A transfer error has occurred.', 'NetworkError'));
      return Promise.reject(e);
    }
  }
  controlTransferIn(setup, length) {
    const bm = this._bm(setup, true);
    return this._run(() => {
      const b = this.sim.ctrl(bm, setup.request, setup.value, setup.index, length);
      return { status: 'ok', data: new DataView(b.buffer, b.byteOffset, b.byteLength) };
    });
  }
  controlTransferOut(setup, data) {
    const bm = this._bm(setup, false);
    return this._run(() => {
      const n = this.sim.ctrl(bm, setup.request, setup.value, setup.index, data ? new Uint8Array(data) : null);
      return { status: 'ok', bytesWritten: n };
    });
  }
}

// navigator.usb stand-in. A DETACH on the app device makes it vanish and the
// bootloader device appear, like re-enumeration.
export class MockUsb {
  constructor(sim) {
    this.sim = sim;
    this.devices = [new MockDevice(sim, sim.mode === 'app' ? 0x1860 : 0x1862)];
    sim.onDetach = () => {
      this.devices = [new MockDevice(sim, 0x1862)];
    };
  }
  async getDevices() { return [...this.devices]; }
}

// sleep() that records into the trace instead of waiting.
export const recordingSleep = (sim) => async (ms) => { sim.trace.push(['sleep', ms]); };

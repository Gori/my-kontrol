// Board table, image validation, release manifest. No DOM.

export const BOARDS = {
  m32: {
    id: 'm32',
    name: 'Komplete Kontrol M32',
    usb: { vid: 0x17cc, appPid: 0x1860, dfuPid: 0x1862 },
    // NI bootloader occupies 0x08000000-0x08002FFF; the application slot ends where
    // NI's knob names start (README "Flash map").
    appBase: 0x08003000,
    appEnd: 0x0801b800,
    maxSize: 0x18800,
    ramStart: 0x20000000,
    ramEnd: 0x20005000,
  },
};

export const hex = (n, w = 8) => '0x' + (n >>> 0).toString(16).toUpperCase().padStart(w, '0');

export function vectors(bytes) {
  if (bytes.length < 8) return null;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return { sp: dv.getUint32(0, true), reset: dv.getUint32(4, true) };
}

// Returns { ok, errors: [string], sp, reset }.
export function validateImage(bytes, board = BOARDS.m32) {
  const errors = [];
  if (!(bytes instanceof Uint8Array) || bytes.length < 8) {
    return { ok: false, errors: ['The file is empty or too small to be firmware.'] };
  }
  if (bytes.length > board.maxSize) {
    errors.push(`Too large: ${bytes.length} bytes, the limit is ${board.maxSize} (${hex(board.maxSize, 5)}).`);
  }
  const { sp, reset } = vectors(bytes);
  if (sp < board.ramStart || sp > board.ramEnd) {
    errors.push(`Initial stack pointer ${hex(sp)} is outside RAM (${hex(board.ramStart)}-${hex(board.ramEnd)}).`);
  }
  if ((reset & 1) === 0) errors.push(`Reset vector ${hex(reset)} is not a Thumb address (must be odd).`);
  if (reset < board.appBase || reset >= board.appEnd) {
    errors.push(`Reset vector ${hex(reset)} is outside the application area (${hex(board.appBase)}-${hex(board.appEnd)}). ` +
      'Was it linked for 0x08003000?');
  }
  return { ok: errors.length === 0, errors, sp, reset };
}

export async function sha256Hex(bytes) {
  const d = await globalThis.crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

const isHex64 = (s) => typeof s === 'string' && /^[0-9a-f]{64}$/i.test(s);

// Parse + sanity-check manifest.json. Releases without a real sha256 (the placeholder)
// are kept but marked unavailable so the UI can say "no release yet".
export function parseManifest(json) {
  if (!json || !Array.isArray(json.boards)) throw new Error('manifest.json: missing "boards"');
  return json.boards.map((b) => {
    const known = BOARDS[b.id] || {};
    const u = b.usb || {};
    const num = (v, d) => (v == null ? d : typeof v === 'number' ? v : parseInt(v, 16));
    const releases = (b.releases || []).map((r) => ({
      ...r,
      available: isHex64(r.sha256) && Number.isInteger(r.size) && r.size > 0 && typeof r.file === 'string',
    }));
    return {
      ...known,
      id: b.id,
      name: b.name || known.name || b.id,
      usb: {
        vid: num(u.vid, known.usb && known.usb.vid),
        appPid: num(u.app_pid, known.usb && known.usb.appPid),
        dfuPid: num(u.dfu_pid, known.usb && known.usb.dfuPid),
      },
      releases,
    };
  });
}

export async function loadManifest(url = 'firmware/manifest.json', fetchFn = globalThis.fetch) {
  const res = await fetchFn(url, { cache: 'no-cache' });
  if (!res.ok) throw new Error(`manifest.json: HTTP ${res.status}`);
  return parseManifest(await res.json());
}

export function latestRelease(board) {
  return (board.releases || []).find((r) => r.available) || null;
}

// Download a release and check size + sha256 against the manifest.
export async function fetchRelease(release, { fetchFn = globalThis.fetch, base } = {}) {
  const url = base ? new URL(release.file, base).href : release.file;
  const res = await fetchFn(url, { cache: 'no-cache' });
  if (!res.ok) throw new Error(`Could not download ${release.file} (HTTP ${res.status})`);
  const bytes = new Uint8Array(await res.arrayBuffer());
  if (bytes.length !== release.size) throw new Error(`Download is ${bytes.length} bytes, expected ${release.size}`);
  const sum = await sha256Hex(bytes);
  if (sum !== release.sha256.toLowerCase()) throw new Error('Download checksum does not match the release');
  return bytes;
}

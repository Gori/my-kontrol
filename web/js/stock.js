// Find NI's stock firmware inside NI's own updater download, in the browser.
// NI's image is never hosted here: the user supplies the updater zip (or a raw .bin).
// No DOM.

import { unzipSync } from '../vendor/fflate.js';
import { sha256Hex } from './images.js';

// Known stock images. NI's updater stores them verbatim inside the executable.
// flashType: the string the image answers to vendor request 0xB3. It is compiled into
// each NI image, so it names the NI firmware family (likely a hardware revision).
// My Kontrol always answers "none" so NI's own updater keeps working.
export const STOCK_IMAGES = [
  {
    board: 'm32',
    label: 'NI firmware for Komplete Kontrol M32',
    version: '1.4.0',
    flashType: 'none', // constant at 0xC753 in this image
    length: 51148,
    sha256: 'b216c750c71d67084065c84d194cd204ad0b2ee19894fecc0b7b4af37074c18e',
    signature: 'd8250020c1310008', // SP 0x200025D8, reset 0x080031C1
    foundIn: ['KKM_FWUpdater_140', 'KKM_FWUpdater_3.17.0'],
  },
  {
    board: 'm32',
    label: 'NI firmware for Komplete Kontrol M32 (type "non3")',
    version: null,
    flashType: 'non3', // constant at 0xE5DC in this image
    length: 58964,
    sha256: 'f3f7c4e8c89ad72c971d969fedef3a1999ea427ff7663650470b82a997cbce51',
    signature: '00260020f1300008', // SP 0x20002600, reset 0x080030F1
    foundIn: ['KKM_FWUpdater_3.17.0'],
  },
];

// Executables inside NI's Mac updater zips (Mac zip works from any OS).
export const UPDATER_EXECUTABLES = [/(^|\/)KKMUpdater\.app\/Contents\/MacOS\/KKMUpdater$/];

export class StockError extends Error {
  constructor(code, message) { super(message); this.name = 'StockError'; this.code = code; }
}

const fromHex = (h) => Uint8Array.from(h.match(/../g), (x) => parseInt(x, 16));

export function isZip(bytes) {
  return bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b && bytes[2] === 0x03 && bytes[3] === 0x04;
}

export function* findAll(hay, needle) {
  const first = needle[0];
  let i = hay.indexOf(first);
  while (i !== -1 && i + needle.length <= hay.length) {
    let k = 1;
    while (k < needle.length && hay[i + k] === needle[k]) k++;
    if (k === needle.length) yield i;
    i = hay.indexOf(first, i + 1);
  }
}

// Search a blob (the updater executable) for every known image of `board`.
// Returns [{ entry, offset, bytes }], one per distinct image, verified by SHA-256.
export async function locateImages(blob, board = 'm32', table = STOCK_IMAGES) {
  const found = [];
  for (const entry of table.filter((e) => e.board === board)) {
    for (const off of findAll(blob, fromHex(entry.signature))) {
      if (off + entry.length > blob.length) continue;
      const bytes = blob.slice(off, off + entry.length);
      if ((await sha256Hex(bytes)) === entry.sha256) {
        found.push({ entry, offset: off, bytes });
        break; // fat Mach-O binaries carry each image once per architecture
      }
    }
  }
  return found;
}

// Unzip only the updater executable(s). Skips macOS "__MACOSX/._*" resource forks.
export function extractExecutables(zipBytes, patterns = UPDATER_EXECUTABLES) {
  let files;
  try {
    files = unzipSync(zipBytes, {
      filter: (f) => !f.name.startsWith('__MACOSX/') && patterns.some((p) => p.test(f.name)),
    });
  } catch (e) {
    throw new StockError('bad-zip', `This zip could not be read (${e.message}).`);
  }
  return Object.entries(files).map(([name, bytes]) => ({ name, bytes }));
}

// Accepts NI's updater zip, an already unpacked executable, or a raw stock .bin.
// Returns [{ entry, bytes, source }] (possibly several images for different flash types).
export async function stockFromFile(bytes, board = 'm32', table = STOCK_IMAGES) {
  const raw = await identify(bytes, board, table);
  if (raw) return [{ entry: raw, bytes, source: 'bin' }];

  let blobs;
  if (isZip(bytes)) {
    blobs = extractExecutables(bytes);
    if (!blobs.length) {
      throw new StockError('no-updater',
        'This zip does not contain KKMUpdater.app. Use the Mac version of NI\'s M32 firmware updater (KKM_FWUpdater_..._Mac.zip).');
    }
  } else {
    blobs = [{ name: 'file', bytes }];
  }
  const out = [];
  for (const b of blobs) {
    for (const f of await locateImages(b.bytes, board, table)) {
      if (!out.some((o) => o.entry.sha256 === f.entry.sha256)) out.push({ ...f, source: b.name });
    }
  }
  if (!out.length) {
    throw new StockError('not-found',
      isZip(bytes)
        ? 'No known M32 firmware was found in this updater. It may be a version this page does not know yet.'
        : 'This file is not a known NI firmware image or updater.');
  }
  return out;
}

export async function identify(bytes, board = 'm32', table = STOCK_IMAGES) {
  const cands = table.filter((e) => e.board === board && e.length === bytes.length);
  if (!cands.length) return null;
  const sum = await sha256Hex(bytes);
  return cands.find((e) => e.sha256 === sum) || null;
}

export const stockName = (e) => `NI firmware${e.version ? ' ' + e.version : ''} / type ${e.flashType}`;

// Which NI type to restore. A live 0xB3 answer other than "none" can only come from NI
// firmware, so it wins; "none" is ambiguous (NI image A or My Kontrol), so then the type
// remembered for this serial, else "none".
export function preferredType({ live = null, remembered = null } = {}) {
  if (live && live !== 'none') return live;
  return remembered || 'none';
}

// Remembered NI types, keyed by USB serial. `storage` is localStorage or similar;
// every access may throw (private mode, blocked storage) and is then ignored.
const KEY = 'mykontrol.flashType.';
export function recallType(storage, serial) {
  if (!serial) return null;
  try { return (storage && storage.getItem(KEY + serial)) || null; } catch { return null; }
}
export function rememberType(storage, serial, type) {
  if (!serial || !type) return false;
  try {
    // "none" may come from My Kontrol; never let it replace a real NI answer.
    if (type === 'none' && recallType(storage, serial)) return false;
    storage.setItem(KEY + serial, type);
    return true;
  } catch { return false; }
}

// Pick the image of the given type; no type -> the 'none' image.
export function chooseStock(found, flashType) {
  if (!found.length) return null;
  if (flashType) return found.find((f) => f.entry.flashType === flashType) || null;
  return found.find((f) => f.entry.flashType === 'none') || found[0];
}

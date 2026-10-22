// Page logic: connect -> choose -> flash -> done.

import * as dfu from './dfu.js';
import { BOARDS, loadManifest, latestRelease, fetchRelease, validateImage, sha256Hex, hex } from './images.js';
import { stockFromFile, identify, preferredType, recallType, rememberType, stockName } from './stock.js';
import * as ui from './ui.js';

const { $ } = ui;
const usb = navigator.usb;

const S = {
  board: { ...BOARDS.m32, releases: [] },
  device: null,
  dfuMode: false,
  flashType: null,
  image: null,      // { bytes, kind: 'mykontrol' | 'stock' | 'file', label }
  pending: null,    // image of the install in progress (kept for Retry)
  wrote: false,     // true once the bootloader has started taking blocks
  busy: false,
  choiceToken: 0,
  onContinue: null,
  stockFile: null,  // { found, name } from the last updater dropped
  customFile: null, // last valid developer .bin
  stockType: null,  // NI type picked by hand under "Advanced"
  mkOverride: false, // developer chose "Install anyway" on an untested hardware type
};

// localStorage may be missing or throw (private mode, blocked site data).
const store = (() => { try { return window.localStorage; } catch { return null; } })();
const ISSUES = 'https://github.com/Gori/my-kontrol/issues';

const vid = () => S.board.usb.vid;
const isApp = (d) => d.vendorId === vid() && d.productId === S.board.usb.appPid;
const isDfu = (d) => d.vendorId === vid() && d.productId === S.board.usb.dfuPid;
const ours = (d) => isApp(d) || isDfu(d);
const filters = () => [
  { vendorId: vid(), productId: S.board.usb.appPid },
  { vendorId: vid(), productId: S.board.usb.dfuPid },
];
const platform = () => ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || '').toLowerCase();
const OLED_DFU = 'Firmware update\nin progress';

// ---------- start ----------

async function init() {
  if (!usb) {
    ui.showView('unsupported');
    ui.setText('unsupported-detail', window.isSecureContext
      ? ''
      : 'If you are already using Chrome or Edge: this page must be opened over https:// or from localhost.');
    return;
  }
  wire();
  ui.showView('connect', { focus: false });
  try {
    const boards = await loadManifest();
    const b = boards.find((x) => x.id === 'm32');
    if (b) S.board = b;
  } catch (e) {
    ui.log(`manifest: ${e.message}`);
  }
  renderRelease();

  // Recovery: a keyboard this browser already knows, sitting in update mode.
  const known = (await usb.getDevices()).filter(ours);
  if (known.some(isDfu)) {
    ui.setText('connect-note', 'Your keyboard is in update mode. Click Connect to finish installing.');
  } else if (known.length === 1) {
    ui.setText('connect-note', `Found ${S.board.name}. ${firmwareName(known[0])}.`);
  }
  usb.addEventListener('disconnect', (e) => {
    if (e.device === S.device && !S.busy && !$('view-choose').hidden) {
      S.device = null;
      ui.showView('connect');
      ui.setText('connect-note', 'The keyboard was disconnected. Plug it back in and click Connect.');
    }
  });
}

function wire() {
  $('btn-connect').addEventListener('click', () => connect(false));
  $('btn-change').addEventListener('click', () => connect(true));
  $('btn-install').addEventListener('click', () => { if (S.image) install(S.image); });
  $('btn-continue').addEventListener('click', () => S.onContinue && S.onContinue());
  $('btn-retry').addEventListener('click', retry);
  $('btn-restart').addEventListener('click', restart);
  $('btn-again').addEventListener('click', restart);
  $('btn-mk-anyway').addEventListener('click', () => {
    const ok = window.confirm(`Install My Kontrol on a keyboard of hardware type “${S.flashType}”? ` +
      'It has never been tested on this type and may not work. NI’s bootloader is not changed, ' +
      'so you can restore NI firmware afterwards.');
    if (ok) { S.mkOverride = true; ui.log(`developer override: My Kontrol on type "${S.flashType}"`); choose('mykontrol'); }
  });
  $('btn-copy-log').addEventListener('click', () => navigator.clipboard && navigator.clipboard.writeText(ui.logText()));
  for (const r of document.querySelectorAll('input[name="choice"]')) r.addEventListener('change', () => choose(r.value));
  ui.dropZone('drop-stock', 'file-stock', onStockFile);
  ui.dropZone('drop-file', 'file-custom', onCustomFile);
  window.addEventListener('beforeunload', (e) => { if (S.busy) { e.preventDefault(); e.returnValue = ''; } });
  // A file dropped anywhere else must not navigate away from the page.
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());
}

function renderRelease() {
  const rel = latestRelease(S.board);
  const radio = document.querySelector('input[value="mykontrol"]');
  if (rel) {
    radio.disabled = false;
    ui.setText('mk-version', `v${rel.version}`);
    if (rel.notes) ui.setText('mk-desc', rel.notes);
  } else {
    radio.disabled = true;
    ui.setText('mk-version', 'coming soon');
    ui.setText('mk-desc', 'No release is published yet. Developers can install a build from file below.');
  }
}

// ---------- step 1: connect ----------

async function connect(forcePicker) {
  ui.setText('connect-note', '');
  let d = null;
  if (!forcePicker) {
    const known = (await usb.getDevices()).filter(ours);
    if (known.length === 1) d = known[0];
  }
  if (!d) {
    try {
      d = await usb.requestDevice({ filters: filters() });
    } catch (e) {
      if (e.name === 'NotFoundError') {
        ui.showView('connect', { focus: false });
        ui.setText('connect-note', 'No keyboard selected. If the M32 is not in the list, check the cable ' +
          '(some cables only charge) and plug it straight into the computer, then click Connect again.');
        return;
      }
      return fail(e, 'connect');
    }
  }
  await useDevice(d);
}

// Display only; never used for safety decisions.
function firmwareName(d) {
  if (isDfu(d)) return 'In update mode (NI bootloader)';
  const mf = d.manufacturerName || '', pn = d.productName || '';
  if (/^my kontrol/i.test(mf) || /oskar|my kontrol/i.test(pn)) return 'Currently installed: My Kontrol';
  if (/native instruments/i.test(mf)) return 'Currently installed: NI firmware';
  return pn ? `Currently installed: “${pn}”` : 'Firmware unknown';
}

async function useDevice(d) {
  ui.setText('connect-note', 'Checking the keyboard…');
  ui.log(`device ${hex(d.vendorId, 4)}:${hex(d.productId, 4)} "${d.manufacturerName || ''}" "${d.productName || ''}" ` +
    `serial ${d.serialNumber || '-'} ` +
    `v${d.deviceVersionMajor}.${d.deviceVersionMinor}.${d.deviceVersionSubminor}`);
  let r;
  try {
    r = await dfu.probe(d);
  } catch (e) {
    S.device = d;
    return fail(e, 'connect');
  }
  ui.log(`dfu interface ${r.info.iface} proto ${r.info.proto} detach ${r.info.timeout} ms xfer ${r.info.xfer}` +
    (r.flashType ? `, flash type "${r.flashType}"` : ''));
  S.device = d;
  S.dfuMode = r.dfuMode;
  S.flashType = r.flashType;
  S.stockType = null;
  S.mkOverride = false;
  ui.setText('connect-note', '');
  ui.setText('device-name', S.board.name);
  ui.setText('device-meta', firmwareName(d));
  ui.setOled('oled-choose', S.dfuMode ? OLED_DFU : 'M32');
  $('dfu-banner').hidden = !S.dfuMode;
  ui.showView('choose');
  const checked = document.querySelector('input[name="choice"]:checked');
  if (checked) {
    choose(checked.value);
  } else if (latestRelease(S.board)) {
    document.querySelector('input[value="mykontrol"]').checked = true;
    choose('mykontrol');
  } else {
    updateInstall();
  }
}

// ---------- step 2: choose ----------

function updateInstall() {
  $('btn-install').disabled = !S.image;
  $('btn-install').textContent = S.image ? `Install ${S.image.short}` : 'Install';
}

async function choose(kind) {
  const token = ++S.choiceToken;
  S.image = null;
  $('sub-stock').hidden = kind !== 'stock';
  $('sub-file').hidden = kind !== 'file';
  $('sub-mk').hidden = kind !== 'mykontrol';
  $('mk-blocked').hidden = true;
  if (kind === 'file') $('advanced').open = true;
  updateInstall();
  if (kind === 'mykontrol') {
    const rel = latestRelease(S.board);
    if (!rel) return ui.setImageStatus('No My Kontrol release is published yet.', 'err');
    // Only NI firmware answers 0xB3 with something other than "none" (My Kontrol always says "none").
    if (S.flashType && S.flashType !== 'none' && !S.mkOverride) {
      ui.setText('mk-blocked-text', `This M32 reports hardware type “${S.flashType}”. My Kontrol has only been tested ` +
        `on type “none”. Please open an issue at ${ISSUES} so we can add it.`);
      $('mk-blocked').hidden = false;
      return ui.setImageStatus('');
    }
    ui.setImageStatus(`Downloading My Kontrol ${rel.version}…`, 'busy');
    try {
      const bytes = await fetchRelease(rel, { base: document.baseURI });
      const v = validateImage(bytes, S.board);
      if (!v.ok) throw new Error(v.errors.join(' '));
      if (token !== S.choiceToken) return;
      setImage({ bytes, kind, label: `My Kontrol ${rel.version}`, short: 'My Kontrol' });
      ui.setImageStatus(`Ready: My Kontrol ${rel.version}, ${ui.kb(bytes.length)}, checksum verified.`, 'ok');
    } catch (e) {
      if (token !== S.choiceToken) return;
      ui.log(`release: ${e.message}`);
      ui.setImageStatus(`Could not get the firmware: ${e.message}. Check your internet connection and try again.`, 'err');
    }
  } else if (kind === 'stock') {
    ui.setImageStatus(S.stockFile ? '' : 'Drop NI’s updater zip above.');
    if (S.stockFile) applyStock(S.stockFile);
  } else if (kind === 'file') {
    ui.setImageStatus(S.customFile ? '' : 'Choose a .bin file above.');
    if (S.customFile) setImage(S.customFile, true);
  }
}

function setImage(img, announceIt = false) {
  S.image = img;
  updateInstall();
  if (announceIt && img.status) ui.setImageStatus(img.status, 'ok');
}

function selectRadio(kind) {
  const r = document.querySelector(`input[value="${kind}"]`);
  if (!r.checked) r.checked = true;
  $('sub-stock').hidden = kind !== 'stock';
  $('sub-file').hidden = kind !== 'file';
  $('sub-mk').hidden = kind !== 'mykontrol';
}

async function readFile(file) {
  return new Uint8Array(await file.arrayBuffer());
}

async function onStockFile(file) {
  selectRadio('stock');
  const token = ++S.choiceToken;
  S.image = null;
  updateInstall();
  ui.setImageStatus(`Looking for the M32 firmware in ${file.name}…`, 'busy');
  try {
    const found = await stockFromFile(await readFile(file), S.board.id);
    if (token !== S.choiceToken) return;
    ui.log(`stock: ${found.map((f) => `${f.entry.flashType} ${f.entry.length} B @${hex(f.offset || 0)} in ${f.source}`).join(', ')}`);
    S.stockFile = { found, name: file.name };
    applyStock(S.stockFile);
  } catch (e) {
    if (token !== S.choiceToken) return;
    S.stockFile = null;
    ui.log(`stock: ${e.message}`);
    ui.setImageStatus(e.message, 'err');
  }
}

// Type to restore: hand-picked > live NI answer > remembered for this serial > "none".
function stockTypeFor() {
  if (S.stockType) return S.stockType;
  const remembered = recallType(store, S.device && S.device.serialNumber);
  return preferredType({ live: S.flashType, remembered });
}

function renderStockTypes(found, type) {
  const box = $('stock-types');
  box.replaceChildren(...found.map((f) => {
    const label = document.createElement('label');
    label.className = 'type-opt';
    const input = document.createElement('input');
    input.type = 'radio';
    input.name = 'stock-type';
    input.value = f.entry.flashType;
    input.checked = f.entry.flashType === type;
    input.addEventListener('change', () => { S.stockType = input.value; applyStock(S.stockFile); });
    label.append(input, document.createTextNode(` ${stockName(f.entry)}`));
    return label;
  }));
  if (found.length < 2) {
    const p = document.createElement('p');
    p.className = 'muted';
    p.textContent = 'This updater contains only one type. NI’s M32 updater 3.17.0 contains both.';
    box.append(p);
  }
  $('stock-adv').hidden = false;
}

function applyStock({ found, name }) {
  const type = stockTypeFor();
  const pick = found.find((f) => f.entry.flashType === type);
  renderStockTypes(found, type);
  if (!pick) {
    $('stock-adv').open = true;
    ui.setImageStatus(`This updater has no NI firmware of type “${type}”, which this keyboard needs. ` +
      'Drop NI’s newest M32 updater (3.17.0 contains both types).', 'err');
    return;
  }
  const v = validateImage(pick.bytes, S.board);
  if (!v.ok) return ui.setImageStatus(v.errors.join(' '), 'err');
  const label = stockName(pick.entry);
  setImage({ bytes: pick.bytes, kind: 'stock', label, short: 'NI firmware' });
  ui.setImageStatus(`Will write: ${label} (${ui.kb(pick.bytes.length)}, checksum verified) from ${name}.`, 'ok');
}

async function onCustomFile(file) {
  selectRadio('file');
  const token = ++S.choiceToken;
  S.image = null;
  S.customFile = null;
  updateInstall();
  const bytes = await readFile(file);
  if (token !== S.choiceToken) return;
  const v = validateImage(bytes, S.board);
  if (!v.ok) {
    ui.setImageStatus(`${file.name} can’t be installed: ${v.errors.join(' ')}`, 'err');
    return;
  }
  const stock = await identify(bytes, S.board.id);
  const sum = await sha256Hex(bytes);
  if (token !== S.choiceToken) return;
  const status = `Ready: ${file.name}${stock ? ' (NI firmware)' : ''}, ${bytes.length} bytes, ` +
    `stack ${hex(v.sp)}, reset ${hex(v.reset)}, SHA-256 ${sum.slice(0, 12)}…`;
  ui.log(`custom image ${file.name} sha256 ${sum}`);
  S.customFile = { bytes, kind: 'file', label: file.name, short: stock ? 'NI firmware' : 'file', status };
  setImage(S.customFile, true);
}

// ---------- step 3: flash ----------

function phase(text, oled) {
  ui.setText('flash-phase', text);
  if (oled != null) ui.setOled('oled-flash', oled);
}

async function install(image) {
  S.pending = image;
  S.wrote = false;
  S.busy = true;
  ui.resetAnnounce();
  $('continue').hidden = true;
  $('flash-warn').hidden = false;
  ui.setText('flash-title', `Installing ${image.label}`);
  ui.showView('flash');
  ui.setProgress(0, ' ', { indeterminate: true });
  try {
    let dev = S.device;
    if (!S.dfuMode && S.flashType && rememberType(store, dev.serialNumber, S.flashType)) {
      ui.log(`remembered type "${S.flashType}" for serial ${dev.serialNumber}`);
    }
    if (!S.dfuMode) {
      phase('Restarting the keyboard in update mode…', 'M32');
      ui.announce('Restarting the keyboard in update mode');
      await dfu.enterDfu(dev, { log: ui.log });
      dev = await waitForBootloader();
      S.device = dev;
      S.dfuMode = true;
    }
    phase('Writing firmware…', OLED_DFU);
    ui.announce('Writing firmware');
    ui.setProgress(0, ' ');
    await dfu.flash(dev, image.bytes, {
      log: ui.log,
      onProgress: (p) => {
        if (p.block > 0) S.wrote = true;
        if (p.phase === 'manifest') {
          phase('Finishing…');
          ui.setProgress(1, `All ${p.blocks} blocks written. Finishing…`);
        } else {
          ui.setProgress(p.bytes / p.total, `Block ${p.block} of ${p.blocks} · ${ui.kb(p.bytes)} of ${ui.kb(p.total)}`);
        }
      },
    });
    S.dfuMode = false;
    phase('Restarting the keyboard…', '');
    ui.setProgress(1, 'Waiting for the keyboard to start…');
    const back = await dfu.waitForDevice(usb, isApp, { tries: 20 });
    S.device = back;
    if (back) await dfu.closeQuietly(back);
    done(image, !!back);
  } catch (e) {
    fail(e, 'flash');
  } finally {
    S.busy = false;
    S.onContinue = null;
  }
}

// After DFU_DETACH: find the bootloader (works when the browser already has permission
// for 17CC:1862), otherwise ask the user to click Continue, which can call requestDevice.
function waitForBootloader() {
  return new Promise((resolve, reject) => {
    let settled = false;
    const ctl = new AbortController();
    const finish = (d) => {
      if (settled) return;
      settled = true;
      ctl.abort();
      clearTimeout(timer);
      $('continue').hidden = true;
      ui.log(`bootloader found: ${d.productName || ''}`);
      resolve(d);
    };
    const showContinue = (text) => {
      if (settled) return;
      if (text) ui.setText('continue-text', text);
      phase('Waiting for your OK…', OLED_DFU);
      $('continue').hidden = false;
      $('btn-continue').focus();
    };
    const timer = setTimeout(() => showContinue(), 2500);
    dfu.waitForDevice(usb, isDfu, { tries: 20, signal: ctl.signal }).then((d) => {
      if (d) finish(d);
      else showContinue('The keyboard should now show “Firmware update in progress”. Click Continue, pick ' +
        'KOMPLETE KONTROL M32 DFU, then click Connect. If the screen shows something else, unplug the keyboard, ' +
        'plug it back in, and click Continue.');
    }, (e) => { if (!settled) { settled = true; reject(e); } });
    S.onContinue = async () => {
      try {
        const d = await usb.requestDevice({ filters: [{ vendorId: vid(), productId: S.board.usb.dfuPid }] });
        finish(d);
      } catch (e) {
        if (e.name === 'NotFoundError') {
          showContinue('Nothing was selected. Click Continue and pick KOMPLETE KONTROL M32 DFU. ' +
            'If it isn’t listed, unplug the keyboard, plug it back in, and try again.');
        } else if (!settled) {
          settled = true;
          ctl.abort();
          reject(e);
        }
      }
    };
  });
}

// ---------- step 4 / errors ----------

function done(image, back) {
  const title = { mykontrol: 'My Kontrol is installed', stock: 'NI firmware restored', file: 'Firmware installed' }[image.kind];
  ui.setText('done-title', title);
  ui.setText('done-text', back
    ? 'The keyboard has restarted and is ready to play.'
    : 'The keyboard restarts on its own. If its screen still says “Firmware update in progress” after 10 seconds, ' +
      'unplug it and plug it back in.');
  ui.setText('done-sub', image.kind === 'stock'
    ? 'Komplete Kontrol and NI’s software work with it as before.'
    : image.kind === 'mykontrol'
      ? 'It shows up as a MIDI device with two ports, “M32 Keys” and “M32 DAW”. No driver or NI software needed.'
      : '');
  S.pending = null;
  ui.showView('done');
  ui.announce(title);
}

function explain(e, stage) {
  const code = e && e.code;
  const mac = platform().includes('mac'), win = platform().includes('win');
  const flashing = stage === 'flash';
  const replug = 'Unplug the keyboard, plug it back in, then click Retry.';

  if (e && e.name === 'SecurityError' && stage === 'connect') {
    return { title: 'The browser blocked access to USB', text: e.message, tips: [
      'Open this page directly (not inside another site) over https:// or from localhost.',
      'If your browser is managed by work or school, USB access may be switched off.'] };
  }
  switch (code) {
    case 'busy': return {
      title: 'Another program is using the keyboard',
      text: 'The browser could not open the M32 because something else has it.',
      tips: [
        'Quit Komplete Kontrol, Native Access, Maschine, your DAW and any other MIDI app.',
        mac ? 'NI Hardware Agent runs in the background: open Activity Monitor, search for “NI”, select NIHardwareAgent and quit it.'
          : win ? 'NI Hardware Agent runs in the background: open Task Manager, find NIHardwareAgent and end it.'
            : 'Close any other program that talks to the keyboard.',
        win ? 'If it still fails, Windows may have NI’s driver attached to the keyboard. See the help page on GitHub.'
          : !mac ? 'On Linux your user needs access to the device (a udev rule; see the help page on GitHub).' : null,
        replug,
      ].filter(Boolean) };
    case 'permission': return {
      title: 'The browser did not allow access',
      text: e.message,
      tips: ['Click Retry and pick the keyboard in the list.',
        platform().includes('linux') ? 'On Linux your user needs access to the device (a udev rule; see the help page on GitHub).' : replug] };
    case 'stall': return {
      title: 'The keyboard refused a command',
      text: flashing && S.wrote ? 'The bootloader is untouched; the keyboard stays in update mode until a good image is written.' : e.message,
      tips: [replug, 'If it happens again, copy the technical details below and open an issue on GitHub.'] };
    case 'timeout': return {
      title: 'The keyboard stopped answering',
      text: flashing && S.wrote
        ? 'The bootloader is untouched; plug the keyboard back in and click Retry. It stays in update mode until a good image is written.'
        : e.message,
      tips: [replug, 'Try another USB port or cable, without a hub.'] };
    case 'disconnected': return {
      title: 'The keyboard was disconnected',
      text: flashing && (S.wrote || S.dfuMode)
        ? 'The bootloader is untouched; plug the keyboard back in and click Retry. It stays in update mode until a good image is written.'
        : 'Plug it back in and click Retry.',
      tips: ['Use a cable that carries data and plug straight into the computer.'] };
    case 'status': return {
      title: 'The keyboard reported a write error',
      text: `${e.message}. The bootloader is untouched, so you can try again.`,
      tips: [replug, 'If it keeps failing, restore NI firmware to check the keyboard.'] };
    case 'not-dfu': return {
      title: 'The keyboard did not enter update mode',
      text: e.message,
      tips: [replug] };
    default: return {
      title: 'Something went wrong',
      text: (e && e.message) || String(e),
      tips: [replug, 'If it happens again, copy the technical details below and open an issue on GitHub.'] };
  }
}

function fail(e, stage) {
  ui.log(`error (${stage}): ${e && e.code ? e.code + ': ' : ''}${(e && e.message) || e}`);
  const m = explain(e, stage);
  ui.showError(m);
  // Keep the warning while the keyboard may still be in update mode.
}

async function retry() {
  try {
    let d = (await usb.getDevices()).filter(ours);
    d = d.find(isDfu) || d.find(isApp) || null;
    if (!d) {
      try {
        d = await usb.requestDevice({ filters: filters() });
      } catch (e) {
        if (e.name === 'NotFoundError') return restartWithNote('Plug the keyboard in and click Connect.');
        throw e;
      }
    }
    if (S.pending) {
      S.device = d;
      S.dfuMode = isDfu(d);
      return install(S.pending);
    }
    await useDevice(d);
  } catch (e) {
    fail(e, S.pending ? 'flash' : 'connect');
  }
}

function restartWithNote(note) {
  S.pending = null;
  ui.showView('connect');
  ui.setText('connect-note', note);
}

async function restart() {
  S.pending = null;
  const still = S.device && (await usb.getDevices()).includes(S.device);
  if (still && !S.busy) return useDevice(S.device);
  S.device = null;
  ui.showView('connect');
}

init();

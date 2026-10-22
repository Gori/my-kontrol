// DOM helpers. All page logic lives in app.js.

export const $ = (id) => document.getElementById(id);

export function initOled() {}

const VIEWS = ['unsupported', 'connect', 'choose', 'flash', 'done', 'error'];
const STEP_OF_VIEW = { connect: 'connect', choose: 'choose', flash: 'flash', done: 'done' };
const ORDER = ['connect', 'choose', 'flash', 'done'];

export function showView(name, { focus = true } = {}) {
  for (const v of VIEWS) $(`view-${v}`).hidden = v !== name;
  const step = STEP_OF_VIEW[name];
  if (step) {
    const idx = ORDER.indexOf(step);
    for (const li of $('steps').children) {
      const i = ORDER.indexOf(li.dataset.step);
      if (i === idx) li.setAttribute('aria-current', 'step'); else li.removeAttribute('aria-current');
      li.classList.toggle('past', i < idx);
    }
  }
  $('steps').hidden = name === 'unsupported';
  if (focus) {
    const h = $(`view-${name}`).querySelector('h3');
    if (h) h.focus({ preventScroll: false });
  }
}

export function setText(id, text) { $(id).textContent = text; }

export function setOled() {}

export function setImageStatus(text, kind = '') {
  const el = $('image-status');
  el.textContent = text;
  el.className = 'image-status' + (kind ? ' ' + kind : '');
}

let lastAnnounced = -1;
export function setProgress(fraction, text, { indeterminate = false } = {}) {
  const p = $('progress');
  p.classList.toggle('indeterminate', indeterminate);
  if (indeterminate) {
    p.removeAttribute('aria-valuenow');
    $('bar').style.width = '';
  } else {
    const pct = Math.round(Math.max(0, Math.min(1, fraction)) * 100);
    p.setAttribute('aria-valuenow', String(pct));
    $('bar').style.width = pct + '%';
    // Screen readers: every 10 %, not every block.
    const step = Math.floor(pct / 10);
    if (step !== lastAnnounced) {
      lastAnnounced = step;
      announce(`${pct} percent`);
    }
  }
  if (text != null) $('progress-text').textContent = text;
}
export function resetAnnounce() { lastAnnounced = -1; }

export function announce(text) { $('announce').textContent = text; }

export function showError({ title, text, tips = [], retry = true }) {
  setText('err-title', title);
  setText('err-text', text);
  const ul = $('err-tips');
  ul.replaceChildren(...tips.map((t) => { const li = document.createElement('li'); li.textContent = t; return li; }));
  $('btn-retry').hidden = !retry;
  showView('error');
}

const logLines = [];
export function log(msg) {
  const t = new Date().toISOString().slice(11, 23);
  logLines.push(`${t} ${msg}`);
  if (logLines.length > 500) logLines.shift();
  $('log').textContent = logLines.join('\n');
  $('log').scrollTop = $('log').scrollHeight;
}
export const logText = () => logLines.join('\n');

// Drop zone + hidden file input. onFile(File).
export function dropZone(zoneId, inputId, onFile) {
  const zone = $(zoneId), input = $(inputId);
  zone.addEventListener('click', () => input.click());
  zone.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); input.click(); }
  });
  input.addEventListener('change', () => { if (input.files[0]) onFile(input.files[0]); input.value = ''; });
  zone.addEventListener('dragover', (e) => { e.preventDefault(); zone.classList.add('over'); });
  zone.addEventListener('dragleave', () => zone.classList.remove('over'));
  zone.addEventListener('drop', (e) => {
    e.preventDefault();
    zone.classList.remove('over');
    const f = e.dataTransfer.files[0];
    if (f) onFile(f);
  });
}

export const kb = (n) => (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';

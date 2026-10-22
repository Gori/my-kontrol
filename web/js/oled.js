// The keyboard's 128x32 screen, drawn pixel for pixel with the firmware's
// own fonts. The page shows what the keyboard would.
import { small, large, huge } from './oled-font.js';

const W = 128, H = 32;
const FONTS = { small, large, huge };

export class Oled {
  constructor(canvas) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.px = new Uint8Array(W * H);
    this.anim = 0;
    this.resize();
    addEventListener('resize', () => { this.resize(); this.flush(); });
    new ResizeObserver(() => { this.resize(); this.flush(); }).observe(canvas);
  }

  resize() {
    const dpr = devicePixelRatio || 1;
    const r = this.canvas.getBoundingClientRect();
    this.canvas.width = Math.max(1, Math.round(r.width * dpr));
    this.canvas.height = Math.max(1, Math.round(r.height * dpr));
  }

  clear() { this.px.fill(0); }
  pixel(x, y, on = 1) { if (x >= 0 && x < W && y >= 0 && y < H) this.px[y * W + x] = on; }
  fill(x, y, w, h, on = 1) { for (let j = y; j < y + h; j++) for (let i = x; i < x + w; i++) this.pixel(i, j, on); }

  width(s, f = 'small') { return s.length * FONTS[f].w; }

  text(x, y, s, f = 'small') {
    const font = FONTS[f];
    for (const ch of s) {
      const g = font.glyphs[(ch.charCodeAt(0) - 32 + 95) % 95] || font.glyphs[31];
      for (let r = 0; r < font.h; r++)
        for (let c = 0; c < font.w; c++)
          if (g[r] & (1 << (font.bits - 1 - c))) this.pixel(x + c, y + r);
      x += font.w;
    }
    return x;
  }

  center(y, s, f = 'small') { this.text(Math.floor((W - this.width(s, f)) / 2), y, s, f); }

  // biggest font that fits the width
  fit(y, s) {
    for (const f of ['huge', 'large', 'small'])
      if (this.width(s, f) <= W - 4) {
        const h = FONTS[f].h;
        this.center(y ?? Math.floor((H - h) / 2), s, f);
        return FONTS[f].h;
      }
    this.center(y ?? 12, s.slice(0, 25), 'small');
    return 8;
  }

  bar(x, y, w, h, frac) {
    this.fill(x, y, w, 1); this.fill(x, y + h - 1, w, 1);
    this.fill(x, y, 1, h); this.fill(x + w - 1, y, 1, h);
    this.fill(x + 2, y + 2, Math.round((w - 4) * Math.max(0, Math.min(1, frac))), h - 4);
  }

  flush() {
    const { ctx, canvas } = this;
    const sx = canvas.width / W, sy = canvas.height / H;
    const css = getComputedStyle(canvas);
    const on = css.getPropertyValue('--pixel').trim() || '#dff2ff';
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = on;
    ctx.shadowColor = on;
    ctx.shadowBlur = sx * 1.6;
    const gap = Math.max(0.5, sx * 0.14);
    for (let y = 0; y < H; y++)
      for (let x = 0; x < W; x++)
        if (this.px[y * W + x]) ctx.fillRect(x * sx, y * sy, sx - gap, sy - gap);
  }

  // A screen: title line in small, a big word, optional bottom line or bar.
  screen({ title = '', big = '', bottom = '', progress = null }) {
    cancelAnimationFrame(this.anim);
    this.clear();
    if (title) this.center(0, title, 'small');
    if (big) {
      const top0 = title ? 8 : 0;
      const room = H - top0 - (bottom || progress != null ? 8 : 0);
      const f = ['huge', 'large', 'small'].find((f) => this.width(big, f) <= W - 4 && FONTS[f].h <= room) || 'small';
      const h = FONTS[f].h;
      this.center(top0 + Math.floor((room - h) / 2), big, f);
    }
    if (progress != null) this.bar(14, 24, W - 28, 8, progress);
    else if (bottom) this.center(24, bottom, 'small');
    this.flush();
  }

  // Boot splash, as on the keyboard, with a short scanline reveal.
  splash(line) {
    cancelAnimationFrame(this.anim);
    this.clear();
    this.center(0, 'My Kontrol', 'huge');
    this.center(24, line, 'small');
    const full = this.px.slice();
    const t0 = performance.now();
    const step = (t) => {
      const rows = Math.min(H, Math.floor((t - t0) / 18));
      this.px.fill(0);
      this.px.set(full.subarray(0, rows * W));
      this.flush();
      if (rows < H) this.anim = requestAnimationFrame(step);
    };
    if (matchMedia('(prefers-reduced-motion: reduce)').matches) { this.px.set(full); this.flush(); }
    else this.anim = requestAnimationFrame(step);
  }
}

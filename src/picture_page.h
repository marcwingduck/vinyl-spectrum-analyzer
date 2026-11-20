#pragma once

// The page at http://vinyl-matrix.local/: pick a picture on the phone, see it as the panel will,
// send it. The phone scales and crops it to 32x16 and posts the raw RGB888 pixels to /picture,
// so the board never decodes an image.

// For the home screen: Android takes the name and icon from here, as astrolab-remote's does.
// The icons aren't cut round, so no "maskable": the launcher's mask would clip the corner LEDs.
static const char PICTURE_MANIFEST[] = R"json({
  "name": "Vinyl Matrix",
  "short_name": "Matrix",
  "start_url": "/",
  "scope": "/",
  "display": "standalone",
  "background_color": "#0d0d12",
  "theme_color": "#0d0d12",
  "icons": [
    { "src": "/icon-192.png", "sizes": "192x192", "type": "image/png" },
    { "src": "/icon-512.png", "sizes": "512x512", "type": "image/png" }
  ]
}
)json";

static const char PICTURE_PAGE[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0d0d12">
<meta name="apple-mobile-web-app-capable" content="yes">
<meta name="apple-mobile-web-app-title" content="Matrix">
<title>Vinyl Matrix</title>
<link rel="manifest" href="/manifest.json">
<link rel="apple-touch-icon" href="/icon-192.png">
<link rel="icon" href="/favicon.png" sizes="32x32">
<style>
:root { color-scheme: dark; --bg: #0d0d12; --card: #17171f; --line: #2a2a36; --text: #ececf1; --dim: #8b8b99; --accent: #e5b5ff; }  /* the bars' lilac */
* { box-sizing: border-box; }
body { margin: 0; min-height: 100vh; min-height: 100dvh; display: flex; justify-content: center; background: var(--bg); color: var(--text);
       font: 16px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
       padding: max(24px, env(safe-area-inset-top)) 16px max(24px, env(safe-area-inset-bottom)); }
main { width: 100%; max-width: 440px; display: flex; flex-direction: column; gap: 16px; }
h1 { margin: 0; font-size: 20px; font-weight: 600; }
/* everything below the title as one block, close together, a little above the middle of the
   free space, where the eye puts the center; the rest of the height goes above and below it */
.stage { flex: 1; display: flex; flex-direction: column; gap: 12px; }
.stage::before { content: ""; flex: 2; }
.stage::after { content: ""; flex: 3; }
canvas { display: block; width: 100%; aspect-ratio: 2 / 1; background: #000; }
canvas.drag { cursor: grab; touch-action: none; }
.hint { margin: 0; font-size: 13px; color: var(--dim); text-align: center; }
.hint[hidden] { display: block; visibility: hidden; }  /* keeps its space, so the preview doesn't jump */
.pick { display: block; padding: 14px; text-align: center; background: var(--card); border: 1px dashed var(--line); border-radius: 12px;
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; cursor: pointer; }
.pick input, .modes input { display: none; }
/* how the picture meets the panel: three parts of one pill, the chosen one filled */
.modes { display: grid; grid-template-columns: repeat(3, 1fr); padding: 4px; border: 1.5px solid var(--line); border-radius: 999px; }
.modes label { padding: 10px 0; text-align: center; font-weight: 600; color: var(--dim); border-radius: 999px; cursor: pointer;
               user-select: none; -webkit-tap-highlight-color: transparent; transition: background .15s, color .15s; }
.modes label.on { color: var(--bg); background: var(--accent); }
button { padding: 14px; font: inherit; font-weight: 600; color: var(--bg); background: var(--accent); border: 0; border-radius: 12px; }
button:disabled { opacity: .35; }
#status { min-height: 1.4em; margin: 0; color: var(--dim); text-align: center; }
</style>
</head>
<body>
<main>
<h1>Vinyl Matrix</h1>
<section class="stage">
<label class="pick"><input type="file" id="file" accept="image/*"><span id="name">Choose image</span></label>
<p class="hint" id="hint" hidden>Drag to adjust crop area</p>
<canvas id="preview" width="640" height="320"></canvas>
<div class="modes">
<label class="on"><input type="radio" name="mode" value="cover" checked>Cover</label>
<label><input type="radio" name="mode" value="contain">Contain</label>
<label><input type="radio" name="mode" value="stretch">Stretch</label>
</div>
<p id="status"></p>
<button id="send" disabled>Send</button>
</section>
</main>
<script>
const W = 32, H = 16, $ = id => document.getElementById(id);
let img = null, pixels = null;

function canvas(w, h) {
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  return c;
}

// Halves step by step down to w x h: one big jump skips most of a photo's pixels and flickers.
// Starts at most 16x the target, so a big photo never needs a canvas of its full size.
function shrink(src, sx, sy, sw, sh, w, h) {
  let f = 1;
  while (f < 16 && w * f * 2 <= sw && h * f * 2 <= sh) f *= 2;
  let c = canvas(w * f, h * f), ctx = c.getContext('2d');
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(src, sx, sy, sw, sh, 0, 0, c.width, c.height);
  while (f > 1) {
    f /= 2;
    const n = canvas(w * f, h * f), nctx = n.getContext('2d');
    nctx.imageSmoothingQuality = 'high';
    nctx.drawImage(c, 0, 0, n.width, n.height);
    c = n;
  }
  return c;
}

// cover: a 2:1 cut fills the panel; contain: all of it, on black; stretch: all of it, squeezed to 2:1
let mode = 'cover';

// Cover cuts a 2:1 part out of the picture, w x h of its pixels. It overflows
// either across or down; spare is how far the cut can move that way, pan where it is (0 the
// left or top edge, 1 the other, the middle to start with).
let pan = .5;
function cut() {
  const sw = img.naturalWidth, sh = img.naturalHeight, w = Math.min(sw, sh * W / H), h = w * H / W;
  return { w, h, across: sw - w >= sh - h, spare: Math.max(sw - w, sh - h) };
}

// Contain shrinks all of it to w x h LEDs, which leaves black LEDs either across or down: spare
// of them, the picture at place along them (0 the left or top edge, 1 the other).
let place = .5;
function fitted() {
  const sw = img.naturalWidth, sh = img.naturalHeight, s = Math.min(W / sw, H / sh);
  const w = Math.max(1, Math.round(sw * s)), h = Math.max(1, Math.round(sh * s));
  return { w, h, across: W - w >= H - h, spare: Math.max(W - w, H - h) };
}

// the panel's 32x16 pixels
function render() {
  const sw = img.naturalWidth, sh = img.naturalHeight, out = canvas(W, H), ctx = out.getContext('2d');
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W, H);
  const c = cut(), f = fitted();
  if (mode == 'contain') {
    const x = Math.round((W - f.w) * place), y = Math.round((H - f.h) * place);
    ctx.drawImage(shrink(img, 0, 0, sw, sh, f.w, f.h), x, y);
  } else if (mode == 'stretch') {
    ctx.drawImage(shrink(img, 0, 0, sw, sh, W, H), 0, 0);
  } else {
    ctx.drawImage(shrink(img, (sw - c.w) * pan, (sh - c.h) * pan, c.w, c.h, W, H), 0, 0);
  }
  // a picture that isn't 2:1 leaves something to drag, unless it's stretched
  const drag = mode != 'stretch' && (mode == 'contain' ? f : c).spare >= 1;
  $('preview').classList.toggle('drag', drag);
  $('hint').hidden = !drag;
  $('hint').textContent = mode == 'contain' ? 'Drag to move' : 'Drag to adjust crop area';
  const rgba = ctx.getImageData(0, 0, W, H).data;
  pixels = new Uint8Array(W * H * 3);
  for (let i = 0; i < W * H; i++) pixels.set(rgba.subarray(i * 4, i * 4 + 3), i * 3);
  draw();
}

// each pixel as an LED, dark where it's off
function draw() {
  const p = $('preview'), ctx = p.getContext('2d'), d = p.width / W;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, p.width, p.height);
  for (let i = 0; i < W * H; i++) {
    const c = pixels ? pixels.subarray(i * 3, i * 3 + 3) : [0, 0, 0];
    ctx.fillStyle = c[0] + c[1] + c[2] ? `rgb(${c[0]},${c[1]},${c[2]})` : '#16161c';
    ctx.beginPath();
    ctx.arc((i % W + .5) * d, ((i / W | 0) + .5) * d, d * .42, 0, 2 * Math.PI);
    ctx.fill();
  }
}

const say = s => $('status').textContent = s;

// An SVG without its own width and height has no real size: browsers report one, but cut a
// part of it out elsewhere, so it landed off center. Sized from its viewBox it draws like any
// image; 1024 px on the longer side is plenty to shrink from.
async function sizedSvg(file) {
  const doc = new DOMParser().parseFromString(await file.text(), 'image/svg+xml'), svg = doc.documentElement;
  if (svg.nodeName !== 'svg') return file;  // not parsed: let the browser try as it is
  const box = (svg.getAttribute('viewBox') || '').trim().split(/[\s,]+/).map(Number);
  const size = a => (/%/.test(svg.getAttribute(a)) ? NaN : parseFloat(svg.getAttribute(a)));
  let w = size('width'), h = size('height');
  if (!(w > 0 && h > 0)) [w, h] = box.length == 4 && box[2] > 0 && box[3] > 0 ? [box[2], box[3]] : [300, 150];
  if (box.length != 4) svg.setAttribute('viewBox', `0 0 ${w} ${h}`);  // so the new size scales it, not cuts it
  const s = 1024 / Math.max(w, h);
  svg.setAttribute('width', w * s);
  svg.setAttribute('height', h * s);
  return new Blob([new XMLSerializer().serializeToString(doc)], { type: 'image/svg+xml' });
}

$('file').onchange = async e => {
  const file = e.target.files[0];
  if (!file) return;
  const svg = file.type == 'image/svg+xml' || /\.svg$/i.test(file.name);
  const next = new Image(), url = URL.createObjectURL(svg ? await sizedSvg(file) : file);
  next.onload = () => {
    URL.revokeObjectURL(url);
    img = next;
    pan = place = .5;
    $('name').textContent = file.name;
    $('send').disabled = false;
    say('');
    render();
  };
  next.onerror = () => { URL.revokeObjectURL(url); say('Not an image'); };
  next.src = url;
};

// a class rather than :has(:checked), which older Android browsers don't know
for (const radio of document.querySelectorAll('input[name=mode]'))
  radio.onchange = () => {
    mode = radio.value;
    for (const r of document.querySelectorAll('input[name=mode]')) r.parentNode.classList.toggle('on', r.checked);
    if (img) render();
  };

// Dragging the preview moves the picture with the finger: the cut over it, or with contain
// the picture between its black LEDs. The preview is the cut's w picture pixels wide, or W LEDs.
let last = null, queued = false;
const preview = $('preview');
preview.onpointerdown = e => {
  if (!preview.classList.contains('drag')) return;
  try { preview.setPointerCapture(e.pointerId); } catch (_) {}  // keeps the drag when the finger leaves it
  last = e;
};
preview.onpointermove = e => {
  if (!last) return;
  const fit = mode == 'contain', l = fit ? fitted() : cut();
  const moved = (l.across ? e.clientX - last.clientX : e.clientY - last.clientY) / preview.clientWidth;
  last = e;
  const clamp = v => Math.min(1, Math.max(0, v));
  if (fit) place = clamp(place + moved * W / l.spare);
  else pan = clamp(pan - moved * l.w / l.spare);
  if (!queued) {  // once per screen refresh, however often the finger reports
    queued = true;
    requestAnimationFrame(() => { queued = false; render(); });
  }
};
preview.onpointerup = preview.onpointercancel = () => last = null;

$('send').onclick = async () => {
  $('send').disabled = true;
  say('Sending…');
  try {
    const r = await fetch('/picture', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: pixels });
    say(r.ok ? 'Sent' : await r.text());
  } catch (e) {
    say('Panel not reachable');
  }
  $('send').disabled = false;
};

draw();
</script>
</body>
</html>
)html";

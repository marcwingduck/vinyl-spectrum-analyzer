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
:root { color-scheme: dark; --bg: #0d0d12; --card: #17171f; --line: #2a2a36; --text: #ececf1; --dim: #8b8b99; --accent: #ff2896; }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text); font: 16px/1.4 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
       display: flex; justify-content: center; padding: max(24px, env(safe-area-inset-top)) 16px max(24px, env(safe-area-inset-bottom)); }
main { width: 100%; max-width: 440px; display: flex; flex-direction: column; gap: 16px; }
h1 { margin: 0; font-size: 20px; font-weight: 600; }
canvas { display: block; width: 100%; aspect-ratio: 2 / 1; background: #000; border: 1px solid var(--line); border-radius: 12px; }
.pick { display: block; padding: 14px; text-align: center; background: var(--card); border: 1px dashed var(--line); border-radius: 12px;
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; cursor: pointer; }
.pick input { display: none; }
.check { display: flex; align-items: center; gap: 10px; color: var(--dim); }
.check input { width: 20px; height: 20px; margin: 0; accent-color: var(--accent); }
button { padding: 14px; font: inherit; font-weight: 600; color: #fff; background: var(--accent); border: 0; border-radius: 12px; }
button:disabled { opacity: .35; }
#status { min-height: 1.4em; margin: 0; color: var(--dim); text-align: center; }
</style>
</head>
<body>
<main>
<h1>Vinyl Matrix</h1>
<canvas id="preview" width="640" height="320"></canvas>
<label class="pick"><input type="file" id="file" accept="image/*"><span id="name">Choose a picture</span></label>
<label class="check"><input type="checkbox" id="fit">Fit the whole picture, with black bars</label>
<button id="send" disabled>Show on the panel</button>
<p id="status"></p>
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

// the panel's 32x16 pixels: the middle cut to 2:1, or with fit all of it, centered on black
function render() {
  const sw = img.naturalWidth, sh = img.naturalHeight, out = canvas(W, H), ctx = out.getContext('2d');
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W, H);
  if ($('fit').checked) {
    const s = Math.min(W / sw, H / sh), w = Math.max(1, Math.round(sw * s)), h = Math.max(1, Math.round(sh * s));
    ctx.drawImage(shrink(img, 0, 0, sw, sh, w, h), (W - w) >> 1, (H - h) >> 1);
  } else {
    const cw = Math.min(sw, sh * W / H), ch = cw * H / W;
    ctx.drawImage(shrink(img, (sw - cw) / 2, (sh - ch) / 2, cw, ch, W, H), 0, 0);
  }
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
    $('name').textContent = file.name;
    $('send').disabled = false;
    say('');
    render();
  };
  next.onerror = () => { URL.revokeObjectURL(url); say("Can't read that picture."); };
  next.src = url;
};

$('fit').onchange = () => img && render();

$('send').onclick = async () => {
  $('send').disabled = true;
  say('Sending…');
  try {
    const r = await fetch('/picture', { method: 'POST', headers: { 'Content-Type': 'application/octet-stream' }, body: pixels });
    say(r.ok ? 'On the panel.' : await r.text());
  } catch (e) {
    say("The panel doesn't answer. Same WiFi?");
  }
  $('send').disabled = false;
};

draw();
</script>
</body>
</html>
)html";

// Canvas port of the native Cairo renderer (src/goose.cpp Goose::Draw and
// src/cosmetics.cpp). Units are the app's own local units at globalScale 1,
// with the goose standing at (0, 0) and +y pointing down the screen.

export const INK = '#141417';
const ORANGE = '#ffa300';
const RIM = '#d1d1d1';
const ISO = { x: 1.3, y: 0.4 };
const BEAK_LEN = 12;
const BEAK_WID = 9;
const BEAK_BASE_OFFSET = 4;

export const STYLE_META = {
  'baseball-cap': { slot: 'hat', label: 'Cap' },
  'top-hat': { slot: 'hat', label: 'Top hat' },
  'party-hat': { slot: 'hat', label: 'Party hat' },
  'crown': { slot: 'hat', label: 'Crown' },
  'round': { slot: 'glasses', label: 'Round' },
  'shades': { slot: 'glasses', label: 'Shades' },
  'aviators': { slot: 'glasses', label: 'Aviators' },
};

const rad = (deg) => (deg * Math.PI) / 180;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const lerp = (a, b, t) => a + (b - a) * t;
const add = (a, b) => ({ x: a.x + b.x, y: a.y + b.y });
const mul = (a, k) => ({ x: a.x * k, y: a.y * k });
const iso = (deg) => ({ x: Math.cos(rad(deg)) * ISO.x, y: Math.sin(rad(deg)) * ISO.y });

function normalize(v) {
  const length = Math.hypot(v.x, v.y) || 1;
  return { x: v.x / length, y: v.y / length };
}

function withAlpha(hex, alpha) {
  const n = parseInt(hex.slice(1), 16);
  return `rgba(${(n >> 16) & 255}, ${(n >> 8) & 255}, ${n & 255}, ${alpha})`;
}

// Goose::UpdateRig for a standing goose; neckLerp 1 is the run-stretch pose.
function computeRig(dir, neckLerp) {
  const fwd = iso(dir);
  const back = clamp(-normalize(fwd).y, 0, 1);
  const body = { x: 0, y: -14 };
  const underbody = { x: 0, y: -9 };
  const neckH = lerp(20, 10, neckLerp);
  const neckExt = lerp(3, 16, neckLerp);
  const neckBase = add(add(body, mul(fwd, 15)), { x: 0, y: -2 * back });
  const neckHead = add(add(neckBase, mul(fwd, neckExt + 2 * back)), { x: 0, y: -neckH });
  return {
    fwd, back, body, underbody, neckBase, neckHead,
    head1: add(neckHead, mul(fwd, 2)),
    head2: add(neckHead, mul(fwd, 4)),
    side: iso(dir + 90),
    lFoot: mul(iso(dir - 90), 4),
    rFoot: mul(iso(dir + 90), 4),
  };
}

function ellipse(ctx, p, rx, ry, fill) {
  ctx.save();
  ctx.translate(p.x, p.y);
  ctx.scale(rx, ry);
  ctx.beginPath();
  ctx.arc(0, 0, 1, 0, Math.PI * 2);
  ctx.restore();
  ctx.fillStyle = fill;
  ctx.fill();
}

function line(ctx, a, b, width) {
  ctx.lineWidth = width;
  ctx.beginPath();
  ctx.moveTo(a.x, a.y);
  ctx.lineTo(b.x, b.y);
  ctx.stroke();
}

function bentLine(ctx, a, control, b, width) {
  const c1 = add(a, mul({ x: control.x - a.x, y: control.y - a.y }, 2 / 3));
  const c2 = add(b, mul({ x: control.x - b.x, y: control.y - b.y }, 2 / 3));
  ctx.lineWidth = width;
  ctx.beginPath();
  ctx.moveTo(a.x, a.y);
  ctx.bezierCurveTo(c1.x, c1.y, c2.x, c2.y, b.x, b.y);
  ctx.stroke();
}

function roundedRect(ctx, x, y, w, h, r) {
  const right = x + w;
  const bottom = y + h;
  ctx.beginPath();
  ctx.arc(right - r, y + r, r, -Math.PI / 2, 0);
  ctx.arc(right - r, bottom - r, r, 0, Math.PI / 2);
  ctx.arc(x + r, bottom - r, r, Math.PI / 2, Math.PI);
  ctx.arc(x + r, y + r, r, Math.PI, Math.PI * 1.5);
  ctx.closePath();
}

function fillAndStroke(ctx, fill, width = 1.2) {
  ctx.fillStyle = fill;
  ctx.fill();
  ctx.strokeStyle = INK;
  ctx.lineWidth = width;
  ctx.stroke();
}

function baseballCap(ctx, item, pose) {
  ctx.beginPath();
  ctx.moveTo(-8, 0);
  ctx.bezierCurveTo(-7, -9, 7, -9, 8, 0);
  ctx.closePath();
  fillAndStroke(ctx, item.primary);

  const billDirection = pose.forward.x < -0.1 ? -1 : 1;
  ctx.save();
  ctx.translate(billDirection * 7, 0.5);
  ctx.scale(1, 0.35);
  ctx.beginPath();
  ctx.arc(0, 0, 7, 0, Math.PI * 2);
  ctx.restore();
  fillAndStroke(ctx, item.accent, 1);

  ctx.strokeStyle = item.accent;
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(0, -7);
  ctx.lineTo(0, -1);
  ctx.stroke();
}

function topHat(ctx, item) {
  roundedRect(ctx, -7, -14, 14, 13, 2);
  fillAndStroke(ctx, item.primary);
  ctx.fillStyle = item.accent;
  ctx.fillRect(-7, -4, 14, 3);
  roundedRect(ctx, -11, -2, 22, 4, 2);
  fillAndStroke(ctx, item.primary);
}

function partyHat(ctx, item) {
  ctx.beginPath();
  ctx.moveTo(-8, 0);
  ctx.lineTo(0, -18);
  ctx.lineTo(8, 0);
  ctx.closePath();
  fillAndStroke(ctx, item.primary);

  ctx.strokeStyle = item.accent;
  ctx.lineWidth = 2.2;
  ctx.beginPath();
  ctx.moveTo(-5, -5);
  ctx.lineTo(5, -9);
  ctx.stroke();
  ctx.fillStyle = item.accent;
  ctx.beginPath();
  ctx.arc(0, -19, 2.5, 0, Math.PI * 2);
  ctx.fill();
}

function crown(ctx, item) {
  ctx.beginPath();
  ctx.moveTo(-9, 1);
  ctx.lineTo(-9, -10);
  ctx.lineTo(-4, -5);
  ctx.lineTo(0, -13);
  ctx.lineTo(4, -5);
  ctx.lineTo(9, -10);
  ctx.lineTo(9, 1);
  ctx.closePath();
  fillAndStroke(ctx, item.primary);
  ctx.fillStyle = item.accent;
  for (const x of [-5, 0, 5]) {
    ctx.beginPath();
    ctx.arc(x, -1.5, 1.2, 0, Math.PI * 2);
    ctx.fill();
  }
}

// ---- Drawn items (cosmetics.cpp: AppendShapePath, PaintShape, DrawCustom*) ----

const mid = (a, b) => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2];

export function shapePath(shape, forceClosed) {
  const path = new Path2D();
  const p = shape.points;
  if (shape.kind === 'ellipse') {
    path.ellipse(p[0][0], p[0][1], p[1][0], p[1][1], 0, 0, Math.PI * 2);
  } else if (shape.kind === 'rect') {
    const [x, y] = p[0];
    const [w, h] = p[1];
    const r = Math.min(shape.radius, Math.min(w, h) / 2);
    if (r > 0.01) {
      path.arc(x + w - r, y + r, r, -Math.PI / 2, 0);
      path.arc(x + w - r, y + h - r, r, 0, Math.PI / 2);
      path.arc(x + r, y + h - r, r, Math.PI / 2, Math.PI);
      path.arc(x + r, y + r, r, Math.PI, Math.PI * 1.5);
      path.closePath();
    } else {
      path.rect(x, y, w, h);
    }
  } else {
    const n = p.length;
    const closed = forceClosed || shape.closed;
    if (shape.smooth && n > 2 && closed) {
      const start = mid(p[n - 1], p[0]);
      path.moveTo(start[0], start[1]);
      for (let i = 0; i < n; i++) {
        const end = mid(p[i], p[(i + 1) % n]);
        path.quadraticCurveTo(p[i][0], p[i][1], end[0], end[1]);
      }
      path.closePath();
    } else if (shape.smooth && n > 2) {
      path.moveTo(p[0][0], p[0][1]);
      for (let i = 1; i + 1 < n; i++) {
        const end = mid(p[i], p[i + 1]);
        path.quadraticCurveTo(p[i][0], p[i][1], end[0], end[1]);
      }
      path.lineTo(p[n - 1][0], p[n - 1][1]);
    } else {
      path.moveTo(p[0][0], p[0][1]);
      for (let i = 1; i < n; i++) path.lineTo(p[i][0], p[i][1]);
      if (closed) path.closePath();
    }
  }
  return path;
}

export function paintShape(ctx, item, shape) {
  const color = shape.role === 'accent' ? item.accent : item.primary;
  ctx.save();
  if (shape.paint === 'line') {
    ctx.strokeStyle = color;
    ctx.lineWidth = shape.width > 0 ? shape.width : 1.4;
    ctx.lineCap = 'round';
    ctx.lineJoin = 'round';
    ctx.stroke(shapePath(shape, false));
  } else if (shape.paint === 'glass') {
    const path = shapePath(shape, true);
    ctx.fillStyle = withAlpha(item.accent, 0.26);
    ctx.fill(path);
    ctx.strokeStyle = item.primary;
    ctx.lineWidth = shape.width > 0 ? shape.width : 1.5;
    ctx.stroke(path);
  } else if (shape.paint === 'flat') {
    ctx.fillStyle = color;
    ctx.fill(shapePath(shape, true));
  } else {
    const path = shapePath(shape, true);
    ctx.fillStyle = color;
    ctx.fill(path);
    ctx.strokeStyle = INK;
    ctx.lineWidth = shape.width > 0 ? shape.width : 1.2;
    ctx.lineJoin = 'round';
    ctx.stroke(path);
  }
  ctx.restore();
}

function drawCustomHat(ctx, item, pose) {
  const facingLeft = pose.forward.x < -0.1;
  for (const shape of item.shapes) {
    ctx.save();
    if (shape.flip && facingLeft) ctx.scale(-1, 1);
    paintShape(ctx, item, shape);
    ctx.restore();
  }
}

// Paired shapes repeat once per eye, mirrored on the left, and collapse to one
// centered copy when the goose faces away.
function drawCustomGlasses(ctx, item, single, separation) {
  for (const shape of item.shapes) {
    if (shape.pair) {
      ctx.save();
      ctx.translate(separation, 0);
      paintShape(ctx, item, shape);
      ctx.restore();
      if (!single) {
        ctx.save();
        ctx.translate(-separation, 0);
        ctx.scale(-1, 1);
        paintShape(ctx, item, shape);
        ctx.restore();
      }
    } else if (!(shape.bridge && single)) {
      paintShape(ctx, item, shape);
    }
  }
}

function drawHat(ctx, item, pose) {
  ctx.save();
  ctx.translate(pose.head.x, pose.head.y - 9);
  if (item.style === 'baseball-cap') baseballCap(ctx, item, pose);
  else if (item.style === 'top-hat') topHat(ctx, item);
  else if (item.style === 'party-hat') partyHat(ctx, item);
  else if (item.style === 'crown') crown(ctx, item);
  else if (item.style === 'custom') drawCustomHat(ctx, item, pose);
  ctx.restore();
}

function roundLens(ctx, x, item) {
  ctx.beginPath();
  ctx.arc(x, 0, 4.2, 0, Math.PI * 2);
  ctx.fillStyle = withAlpha(item.accent, 0.26);
  ctx.fill();
  ctx.strokeStyle = item.primary;
  ctx.lineWidth = 1.5;
  ctx.stroke();
}

function drawGlasses(ctx, item, pose) {
  const side = normalize(pose.side);
  const eyeLift = 3 + 1.5 * pose.back;
  const angle = Math.atan2(side.y, side.x);
  const single = pose.back > 0.82;
  const separation = single ? 0 : 5 - 2.2 * pose.back;

  ctx.save();
  ctx.translate(pose.head.x, pose.head.y - eyeLift);
  ctx.rotate(angle);

  if (item.style === 'custom') {
    drawCustomGlasses(ctx, item, single, separation);
    ctx.restore();
    return;
  }

  if (item.style === 'round') {
    roundLens(ctx, -separation, item);
    if (!single) roundLens(ctx, separation, item);
  } else if (item.style === 'shades') {
    const lensWidth = single ? 8.5 : 7.5;
    roundedRect(ctx, -separation - lensWidth / 2, -3.2, lensWidth, 6.4, 1.5);
    fillAndStroke(ctx, item.primary, 1);
    if (!single) {
      roundedRect(ctx, separation - lensWidth / 2, -3.2, lensWidth, 6.4, 1.5);
      fillAndStroke(ctx, item.primary, 1);
    }
  } else if (item.style === 'aviators') {
    for (const x of single ? [-separation] : [-separation, separation]) {
      ctx.save();
      ctx.translate(x, 0);
      ctx.scale(1, 0.82);
      roundLens(ctx, 0, item);
      ctx.restore();
    }
  }

  if (!single) {
    ctx.strokeStyle = item.primary;
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    ctx.moveTo(-separation + 4, 0);
    ctx.lineTo(separation - 4, 0);
    ctx.stroke();
  }
  ctx.restore();
}

// Cosmetics_Draw. `pop` scales both items about the head for a bounce.
function drawCosmetics(ctx, skin, pose, pop) {
  ctx.save();
  ctx.lineCap = 'butt';
  ctx.lineJoin = 'miter';
  if (pop !== 1) {
    ctx.translate(pose.head.x, pose.head.y);
    ctx.scale(pop, pop);
    ctx.translate(-pose.head.x, -pose.head.y);
  }
  if (skin.hat) drawHat(ctx, skin.hat, pose);
  if (skin.glasses) drawGlasses(ctx, skin.glasses, pose);
  ctx.restore();
}

// Goose::Draw, standing pose. `ctx` must already be at the goose's position.
export function drawGoose(ctx, { dir, neckLerp = 0, skin, pop = 1 }) {
  const r = computeRig(dir, neckLerp);
  const { fwd, back } = r;

  ctx.save();
  ellipse(ctx, { x: 2, y: 10 }, 20, 15, 'rgba(0, 0, 0, 0.3)');
  ellipse(ctx, r.lFoot, 4, 4, ORANGE);
  ellipse(ctx, r.rFoot, 4, 4, ORANGE);

  ctx.save();
  ctx.translate(r.body.x, r.body.y);
  ctx.scale(1, lerp(1, 0.92, back));
  ctx.translate(-r.body.x, -r.body.y);

  const bodyFront = add(r.body, mul(fwd, 11));
  const bodyBack = add(r.body, mul(fwd, -11));
  const underFront = add(r.underbody, mul(fwd, 7));
  const underBack = add(r.underbody, mul(fwd, -7));

  ctx.lineCap = 'round';
  ctx.strokeStyle = RIM;
  bentLine(ctx, bodyFront, r.body, bodyBack, 24);
  line(ctx, r.neckBase, r.neckHead, 15);
  line(ctx, r.neckHead, r.head1, 17);
  line(ctx, r.head1, r.head2, 12);
  bentLine(ctx, underFront, r.underbody, underBack, 15);

  const beakBase = add(r.neckHead, mul(fwd, BEAK_BASE_OFFSET));
  ctx.strokeStyle = ORANGE;
  line(ctx, beakBase, add(beakBase, mul(fwd, BEAK_LEN)), BEAK_WID);

  ctx.strokeStyle = '#fff';
  bentLine(ctx, bodyFront, r.body, bodyBack, 22);
  line(ctx, r.neckBase, r.neckHead, 13);
  line(ctx, r.neckHead, r.head1, 15);
  line(ctx, r.head1, r.head2, 10);
  ctx.restore();

  // Eyes
  const eyeSep = lerp(5, 2.8, back);
  const eyeCenter = { x: r.neckHead.x, y: r.neckHead.y - (3 + lerp(0, 1.5, back)) };
  if (back > 0.82) {
    ellipse(ctx, eyeCenter, 2, 2, '#000');
  } else {
    ellipse(ctx, add(eyeCenter, mul(r.side, -eyeSep)), 2, 2, '#000');
    ellipse(ctx, add(eyeCenter, mul(r.side, eyeSep)), 2, 2, '#000');
  }

  drawCosmetics(ctx, skin, { head: r.neckHead, forward: fwd, side: r.side, back }, pop);
  ctx.restore();
}

// `fixedDpr` renders an offscreen canvas at exactly its own pixel size.
export function setupCanvas(canvas, fixedDpr = 0) {
  const dpr = fixedDpr || Math.min(window.devicePixelRatio || 1, 3);
  const width = canvas.clientWidth || canvas.width;
  const height = canvas.clientHeight || canvas.height;
  const pixelWidth = Math.max(1, Math.round(width * dpr));
  const pixelHeight = Math.max(1, Math.round(height * dpr));
  if (canvas.width !== pixelWidth || canvas.height !== pixelHeight) {
    canvas.width = pixelWidth;
    canvas.height = pixelHeight;
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  return { ctx, width, height };
}

// The goose spans roughly x -45..50, y -72..28 once the neck stretches.
const FIT_W = 100;
const FIT_H = 104;

export function drawStage(canvas, state, fill = 0.94, fixedDpr = 0) {
  const { ctx, width, height } = setupCanvas(canvas, fixedDpr);
  const scale = Math.min(width / FIT_W, height / FIT_H) * fill;
  ctx.translate(width / 2, height / 2);
  ctx.scale(scale, scale);
  ctx.translate(-3, 22);
  drawGoose(ctx, state);
}

// A single cosmetic on its own, facing forward, for the picker tiles.
export function drawIcon(canvas, item) {
  const { ctx, width, height } = setupCanvas(canvas);
  const isHat = (item.slot || STYLE_META[item.style].slot) === 'hat';
  const scale = Math.min(width, height) / (isHat ? 30 : 18) * 0.9;
  ctx.translate(width / 2, height / 2);
  ctx.scale(scale, scale);
  const pose = { forward: { x: 1, y: 0 }, side: { x: 1, y: 0 }, back: 0 };
  if (isHat) drawHat(ctx, item, { ...pose, head: { x: 0, y: 17 } });
  else drawGlasses(ctx, item, { ...pose, head: { x: 0, y: 3 } });
}

// The "draw your own" tile.
export function drawPencilIcon(canvas) {
  const { ctx, width, height } = setupCanvas(canvas);
  const s = Math.min(width, height) / 40;
  ctx.translate(width / 2, height / 2);
  ctx.scale(s, s);
  ctx.rotate(Math.PI / 4);
  ctx.lineJoin = 'round';
  ctx.lineWidth = 1.8;
  ctx.strokeStyle = INK;
  ctx.beginPath();
  ctx.rect(-3.5, -13, 7, 19);
  ctx.fillStyle = '#ffa300';
  ctx.fill();
  ctx.stroke();
  ctx.beginPath();
  ctx.rect(-3.5, -13, 7, 4);
  ctx.fillStyle = '#f4a0a0';
  ctx.fill();
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(-3.5, 6);
  ctx.lineTo(0, 13);
  ctx.lineTo(3.5, 6);
  ctx.closePath();
  ctx.fillStyle = '#fffff0';
  ctx.fill();
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(-1.2, 10.3);
  ctx.lineTo(0, 13);
  ctx.lineTo(1.2, 10.3);
  ctx.fillStyle = INK;
  ctx.fill();
}

// The "no item" tile: a crossed-out ring.
export function drawNoneIcon(canvas) {
  const { ctx, width, height } = setupCanvas(canvas);
  const r = Math.min(width, height) * 0.28;
  ctx.translate(width / 2, height / 2);
  ctx.strokeStyle = INK;
  ctx.lineWidth = 2.5;
  ctx.lineCap = 'round';
  ctx.beginPath();
  ctx.arc(0, 0, r, 0, Math.PI * 2);
  ctx.moveTo(-r * 0.7, r * 0.7);
  ctx.lineTo(r * 0.7, -r * 0.7);
  ctx.stroke();
}

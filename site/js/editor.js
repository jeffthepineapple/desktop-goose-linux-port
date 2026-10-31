// The drawing workshop: pen, oval, box, polygon, move and erase over a
// goose-head guide, in the same local units the native renderer draws in.

import { paintShape, shapePath, setupCanvas, INK } from './render.js';
import { MAX_SHAPES, MAX_POINTS, TEMPLATES, parseShape, simplify, cloneShapes } from './shapes.js';

const SEP = 5; // lens offset when the goose faces the camera
const VIEWS = {
  hat: { x0: -20, x1: 20, y0: -30, y1: 20, head: 9 },
  glasses: { x0: -20, x1: 20, y0: -12, y1: 18, head: 3 },
};
const HELP = {
  pen: 'Drag to draw. Let go and the stroke becomes a shape.',
  oval: 'Drag across the shape you want.',
  box: 'Drag across the shape you want.',
  poly: 'Click each corner. Click the first dot, double-click or press Enter to finish. Backspace undoes a corner.',
  move: 'Click a shape to select it, drag to move it, Delete removes it. Arrow keys nudge.',
  erase: 'Click a shape to remove it.',
};

const snapTo = (value) => Math.round(value * 2) / 2;

export function createEditor({ canvas, status, host, onChange }) {
  const scratch = document.createElement('canvas').getContext('2d');
  let slot = null;
  let scale = 1;
  let ox = 0;
  let oy = 0;
  let hover = null;
  let draft = null; // in-progress pen stroke or oval/box drag
  let poly = null;  // in-progress polygon corners
  let move = null;
  let selected = -1;
  let undoStack = [];
  let redoStack = [];

  const opts = {
    tool: 'pen', role: 'primary', paint: 'fill',
    smooth: true, snap: true, mirror: true, flip: false, pair: true, bridge: false,
  };

  const shapes = () => host.getShapes(slot);
  const say = (text) => { status.textContent = text; };

  // ---- Geometry ----------------------------------------------------------

  function layout() {
    const view = VIEWS[slot];
    const width = canvas.clientWidth;
    const height = canvas.clientHeight;
    scale = Math.min(width / (view.x1 - view.x0), height / (view.y1 - view.y0));
    ox = width / 2 - ((view.x0 + view.x1) / 2) * scale;
    oy = height / 2 - ((view.y0 + view.y1) / 2) * scale;
  }

  function toLocal(event, snap = opts.snap) {
    const rect = canvas.getBoundingClientRect();
    const point = { x: (event.clientX - rect.left - ox) / scale, y: (event.clientY - rect.top - oy) / scale };
    return snap ? { x: snapTo(point.x), y: snapTo(point.y) } : point;
  }

  const isPaired = (shape) => slot === 'glasses' && shape.pair;
  const copiesOf = (shape) => (isPaired(shape) ? [{ dx: SEP, sx: 1 }, { dx: -SEP, sx: -1 }] : [{ dx: 0, sx: 1 }]);

  function bounds(shape) {
    const p = shape.points;
    if (shape.kind === 'ellipse') return [p[0][0] - p[1][0], p[0][1] - p[1][1], p[0][0] + p[1][0], p[0][1] + p[1][1]];
    if (shape.kind === 'rect') return [p[0][0], p[0][1], p[0][0] + p[1][0], p[0][1] + p[1][1]];
    const xs = p.map((q) => q[0]);
    const ys = p.map((q) => q[1]);
    return [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)];
  }

  function translate(shape, dx, dy) {
    if (shape.kind === 'poly') shape.points = shape.points.map(([x, y]) => [x + dx, y + dy]);
    else shape.points[0] = [shape.points[0][0] + dx, shape.points[0][1] + dy];
  }

  function mirrored(shape) {
    const copy = { ...shape, points: shape.points.map((p) => [...p]) };
    if (shape.kind === 'poly') copy.points = copy.points.map(([x, y]) => [-x, y]);
    else if (shape.kind === 'ellipse') copy.points[0] = [-shape.points[0][0], shape.points[0][1]];
    else copy.points[0] = [-(shape.points[0][0] + shape.points[1][0]), shape.points[0][1]];
    const [x0, , x1] = bounds(shape);
    const centered = shape.kind !== 'poly' && Math.abs((x0 + x1) / 2) < 0.3;
    return centered ? null : copy;
  }

  function hit(point) {
    const list = shapes();
    for (let i = list.length - 1; i >= 0; i--) {
      const shape = list[i];
      for (const copy of copiesOf(shape)) {
        const x = (point.x - copy.dx) / copy.sx;
        const path = shapePath(shape, shape.paint !== 'line');
        scratch.lineWidth = Math.max(shape.width || 1.4, 2.4);
        const inside = shape.paint !== 'line' && scratch.isPointInPath(path, x, point.y);
        if (inside || scratch.isPointInStroke(path, x, point.y)) return { index: i, copy };
      }
    }
    return null;
  }

  // ---- History -----------------------------------------------------------

  function record() {
    undoStack.push(cloneShapes(shapes()));
    if (undoStack.length > 100) undoStack.shift();
    redoStack = [];
  }

  function commit(next, message) {
    host.commit(slot, next);
    if (message) say(message);
    syncButtons();
    render();
  }

  function replace(next, message) {
    record();
    commit(next, message);
  }

  function undo() {
    if (!undoStack.length) return;
    redoStack.push(cloneShapes(shapes()));
    selected = -1;
    commit(undoStack.pop(), 'Undid the last change.');
  }

  function redo() {
    if (!redoStack.length) return;
    undoStack.push(cloneShapes(shapes()));
    selected = -1;
    commit(redoStack.pop(), 'Redid the change.');
  }

  function syncButtons() {
    onChange({ canUndo: undoStack.length > 0, canRedo: redoStack.length > 0, hasShapes: shapes().length > 0 });
  }

  // ---- Creating shapes ---------------------------------------------------

  function makeShape(kind, points) {
    const paired = slot === 'glasses' && opts.pair;
    const shift = paired ? -SEP : 0;
    // Only a point is positional; an ellipse's radii and a box's size stay put.
    const shifted = kind === 'poly'
      ? points.map(([x, y]) => [x + shift, y])
      : [[points[0][0] + shift, points[0][1]], points[1]];
    const shape = {
      kind,
      role: opts.role,
      paint: opts.paint,
      pair: paired,
      bridge: slot === 'glasses' && !paired && opts.bridge,
      flip: slot === 'hat' && opts.flip,
      smooth: kind === 'poly' && opts.smooth,
      closed: false,
      width: 0,
      radius: kind === 'rect' && opts.smooth ? 1.5 : 0,
      points: shifted,
    };
    return shape;
  }

  function add(shape, label) {
    const added = [shape];
    if (slot === 'hat' && opts.mirror) {
      const twin = mirrored(shape);
      if (twin) added.push(twin);
    }
    const current = shapes();
    if (current.length + added.length > MAX_SHAPES) {
      say(`A drawing holds up to ${MAX_SHAPES} shapes. Erase one first.`);
      return;
    }
    selected = -1; // options only retarget a shape picked with the Move tool
    replace([...cloneShapes(current), ...added], `Added ${label}.`);
  }

  function finishPen() {
    const raw = draft.points;
    draft = null;
    let points = simplify(raw.map((p) => [p.x, p.y]), 0.22);
    if (points.length > MAX_POINTS) points = simplify(points, 0.5).slice(0, MAX_POINTS);
    const span = Math.hypot(points[points.length - 1][0] - points[0][0], points[points.length - 1][1] - points[0][1]);
    const length = points.reduce((sum, p, i) => (i ? sum + Math.hypot(p[0] - points[i - 1][0], p[1] - points[i - 1][1]) : 0), 0);
    const minimum = opts.paint === 'line' ? 2 : 3;
    if (points.length === 2 && opts.paint !== 'line') {
      // A straight stroke has no area. Give it a hair of thickness instead of dropping it.
      points = [points[0], points[1], [points[1][0] + 0.01, points[1][1] + 0.5]];
    }
    if (length < 1 || points.length < Math.min(minimum, 3) || (opts.paint !== 'line' && span < 0.01 && length < 2)) {
      say('That stroke was too short. Try a longer one.');
      render();
      return;
    }
    if (opts.paint !== 'line' && points.length >= 3) {
      // Closing a drawn outline: drop a last point that sits on the first.
      const [a, b] = [points[0], points[points.length - 1]];
      if (Math.hypot(a[0] - b[0], a[1] - b[1]) < 0.6) points.pop();
    }
    add(makeShape('poly', points), 'a pen shape');
  }

  function finishBox() {
    const { start, cur, kind } = draft;
    draft = null;
    const x0 = Math.min(start.x, cur.x);
    const y0 = Math.min(start.y, cur.y);
    const w = Math.abs(cur.x - start.x);
    const h = Math.abs(cur.y - start.y);
    if (w < 0.8 || h < 0.8) { say('Drag a little further to draw that shape.'); render(); return; }
    if (kind === 'oval') add(makeShape('ellipse', [[x0 + w / 2, y0 + h / 2], [w / 2, h / 2]]), 'an oval');
    else add(makeShape('rect', [[x0, y0], [w, h]]), 'a box');
  }

  function finishPoly(closed) {
    const points = poly.map((p) => [p.x, p.y]);
    poly = null;
    const minimum = opts.paint === 'line' ? 2 : 3;
    if (points.length < minimum) { say(`A polygon needs at least ${minimum} corners.`); render(); return; }
    const shape = makeShape('poly', points);
    shape.closed = opts.paint === 'line' && closed;
    add(shape, 'a polygon');
  }

  // ---- Pointer and keyboard ---------------------------------------------

  function down(event) {
    if (event.button !== 0 || !slot) return;
    canvas.focus({ preventScroll: true });
    const point = toLocal(event);
    canvas.setPointerCapture(event.pointerId);

    if (opts.tool === 'pen') {
      draft = { kind: 'pen', points: [toLocal(event, false)] };
    } else if (opts.tool === 'oval' || opts.tool === 'box') {
      draft = { kind: opts.tool, start: point, cur: point };
    } else if (opts.tool === 'poly') {
      if (!poly) {
        poly = [point];
      } else {
        const first = poly[0];
        const close = poly.length >= 3 && Math.hypot(first.x - point.x, first.y - point.y) * scale < 12;
        if (close) finishPoly(true);
        else if (poly.length < MAX_POINTS) poly.push(point);
      }
    } else if (opts.tool === 'move') {
      const found = hit(toLocal(event, false));
      selected = found ? found.index : -1;
      if (found) {
        move = { start: point, base: cloneShapes(shapes()), sx: found.copy.sx, moved: false };
        reflectSelection();
      }
    } else if (opts.tool === 'erase') {
      const found = hit(toLocal(event, false));
      if (found) {
        selected = -1;
        const next = cloneShapes(shapes());
        next.splice(found.index, 1);
        replace(next, 'Erased a shape.');
      }
    }
    render();
  }

  function moveEvent(event) {
    if (!slot) return;
    hover = toLocal(event);
    if (draft && draft.kind === 'pen') {
      const point = toLocal(event, false);
      const last = draft.points[draft.points.length - 1];
      if (Math.hypot(point.x - last.x, point.y - last.y) > 0.08) draft.points.push(point);
    } else if (draft) {
      draft.cur = hover;
    } else if (move) {
      const dx = (hover.x - move.start.x) * move.sx;
      const dy = hover.y - move.start.y;
      if (dx || dy) move.moved = true;
      const next = cloneShapes(move.base);
      translate(next[selected], dx, dy);
      host.live(slot, next);
    }
    render();
  }

  function up() {
    if (draft) {
      if (draft.kind === 'pen') finishPen();
      else finishBox();
    } else if (move) {
      const done = move;
      move = null;
      if (done.moved) {
        undoStack.push(done.base);
        redoStack = [];
        commit(shapes(), 'Moved the shape.');
      }
    }
    render();
  }

  function nudge(dx, dy) {
    if (selected < 0 || selected >= shapes().length) return;
    const next = cloneShapes(shapes());
    const sign = copiesOf(next[selected])[0].sx;
    translate(next[selected], dx * sign, dy);
    replace(next, 'Nudged the shape.');
  }

  function key(event) {
    if (!slot) return;
    const ctrl = event.ctrlKey || event.metaKey;
    if (ctrl && event.key.toLowerCase() === 'z') {
      event.preventDefault();
      if (event.shiftKey) redo(); else undo();
    } else if (ctrl && event.key.toLowerCase() === 'y') {
      event.preventDefault();
      redo();
    } else if (event.key === 'Enter' && poly) {
      event.preventDefault();
      finishPoly(false);
    } else if (event.key === 'Escape') {
      draft = null; poly = null; move = null; selected = -1;
      render();
    } else if (event.key === 'Backspace' && poly) {
      event.preventDefault();
      poly.pop();
      if (!poly.length) poly = null;
    } else if ((event.key === 'Delete' || event.key === 'Backspace') && selected >= 0) {
      event.preventDefault();
      const next = cloneShapes(shapes());
      next.splice(selected, 1);
      selected = -1;
      replace(next, 'Removed the shape.');
    } else if (event.key.startsWith('Arrow') && selected >= 0) {
      event.preventDefault();
      const step = event.shiftKey ? 2 : 0.5;
      nudge(event.key === 'ArrowLeft' ? -step : event.key === 'ArrowRight' ? step : 0,
        event.key === 'ArrowUp' ? -step : event.key === 'ArrowDown' ? step : 0);
    } else {
      return;
    }
    render();
  }

  canvas.addEventListener('pointerdown', down);
  canvas.addEventListener('pointermove', moveEvent);
  canvas.addEventListener('pointerup', up);
  canvas.addEventListener('pointercancel', up);
  canvas.addEventListener('pointerleave', () => { hover = null; render(); });
  canvas.addEventListener('dblclick', () => { if (poly && poly.length >= 2) { poly.pop(); finishPoly(false); render(); } });
  canvas.addEventListener('keydown', key);

  // ---- Drawing -----------------------------------------------------------

  function withCopy(ctx, copy, draw) {
    ctx.save();
    ctx.translate(copy.dx, 0);
    if (copy.sx < 0) ctx.scale(-1, 1);
    draw();
    ctx.restore();
  }

  function guide(ctx, view) {
    ctx.lineWidth = 1 / scale;
    for (let x = Math.ceil(view.x0); x <= view.x1; x++) {
      ctx.strokeStyle = x % 5 === 0 ? 'rgba(20, 20, 23, 0.16)' : 'rgba(20, 20, 23, 0.06)';
      ctx.beginPath(); ctx.moveTo(x, view.y0); ctx.lineTo(x, view.y1); ctx.stroke();
    }
    for (let y = Math.ceil(view.y0); y <= view.y1; y++) {
      ctx.strokeStyle = y % 5 === 0 ? 'rgba(20, 20, 23, 0.16)' : 'rgba(20, 20, 23, 0.06)';
      ctx.beginPath(); ctx.moveTo(view.x0, y); ctx.lineTo(view.x1, y); ctx.stroke();
    }
    ctx.strokeStyle = 'rgba(20, 20, 23, 0.4)';
    ctx.beginPath(); ctx.moveTo(0, view.y0); ctx.lineTo(0, view.y1); ctx.stroke();
  }

  function head(ctx) {
    const cy = VIEWS[slot].head;
    ctx.save();
    ctx.globalAlpha = 0.9;
    ctx.lineCap = 'round';
    // The goose's own construction: a wider gray stroke under a white one.
    for (const [color, neck, round] of [['#d1d1d1', 15, 17], ['#fff', 13, 15]]) {
      ctx.strokeStyle = color;
      ctx.lineWidth = neck;
      ctx.beginPath(); ctx.moveTo(0, cy); ctx.lineTo(0, cy + 40); ctx.stroke();
      ctx.lineWidth = round;
      ctx.beginPath(); ctx.moveTo(0, cy); ctx.lineTo(0, cy + 0.01); ctx.stroke();
    }
    ctx.fillStyle = '#ffa300';
    ctx.beginPath(); ctx.ellipse(0, cy + 4.5, 4.5, 3, 0, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#000';
    for (const x of [-SEP, SEP]) {
      ctx.beginPath(); ctx.arc(x, cy - 3, 2, 0, Math.PI * 2); ctx.fill();
    }
    ctx.restore();
  }

  function bbox(ctx, shape) {
    const [x0, y0, x1, y1] = bounds(shape);
    const pad = 0.8;
    for (const copy of copiesOf(shape)) {
      withCopy(ctx, copy, () => {
        ctx.lineWidth = 2 / scale;
        ctx.setLineDash([4 / scale, 3 / scale]);
        ctx.strokeStyle = '#fff';
        ctx.strokeRect(x0 - pad, y0 - pad, x1 - x0 + pad * 2, y1 - y0 + pad * 2);
        ctx.lineDashOffset = 3.5 / scale;
        ctx.strokeStyle = INK;
        ctx.strokeRect(x0 - pad, y0 - pad, x1 - x0 + pad * 2, y1 - y0 + pad * 2);
        ctx.lineDashOffset = 0;
        ctx.setLineDash([]);
      });
    }
  }

  function render() {
    if (!slot) return;
    const { ctx, width, height } = setupCanvas(canvas);
    layout();
    const view = VIEWS[slot];
    ctx.fillStyle = 'rgba(255, 255, 240, 0.9)';
    ctx.fillRect(0, 0, width, height);
    ctx.save();
    ctx.translate(ox, oy);
    ctx.scale(scale, scale);
    guide(ctx, view);
    head(ctx);

    // Writing area the renderer is tuned for
    ctx.setLineDash([3 / scale, 3 / scale]);
    ctx.strokeStyle = 'rgba(20, 20, 23, 0.45)';
    ctx.lineWidth = 1.2 / scale;
    const box = slot === 'hat' ? [-16, -20, 32, 24] : [-12, -6, 24, 12];
    ctx.strokeRect(...box);
    ctx.setLineDash([]);

    const colors = host.colors(slot);
    const item = { primary: colors.primary, accent: colors.accent };
    shapes().forEach((shape) => {
      for (const copy of copiesOf(shape)) withCopy(ctx, copy, () => paintShape(ctx, item, shape));
    });
    if (selected >= 0 && shapes()[selected]) bbox(ctx, shapes()[selected]);

    // Rubber-banding
    ctx.globalAlpha = 0.75;
    if (draft && draft.kind === 'pen' && draft.points.length > 1) {
      ctx.strokeStyle = opts.role === 'accent' ? colors.accent : colors.primary;
      ctx.lineWidth = 1.2;
      ctx.lineCap = 'round';
      ctx.lineJoin = 'round';
      ctx.beginPath();
      draft.points.forEach((p, i) => (i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
      ctx.stroke();
    } else if (draft && draft.kind !== 'pen') {
      const { start, cur } = draft;
      const w = Math.abs(cur.x - start.x);
      const h = Math.abs(cur.y - start.y);
      if (w > 0.1 && h > 0.1) {
        const x0 = Math.min(start.x, cur.x);
        const y0 = Math.min(start.y, cur.y);
        const preview = draft.kind === 'oval'
          ? { kind: 'ellipse', role: opts.role, paint: opts.paint, points: [[x0 + w / 2, y0 + h / 2], [w / 2, h / 2]], radius: 0, width: 0 }
          : { kind: 'rect', role: opts.role, paint: opts.paint, points: [[x0, y0], [w, h]], radius: opts.smooth ? 1.5 : 0, width: 0 };
        paintShape(ctx, item, preview);
      }
    }
    if (poly) {
      const live = hover ? [...poly, hover] : poly;
      ctx.strokeStyle = INK;
      ctx.lineWidth = 1.2 / scale * 1.4;
      ctx.setLineDash([4 / scale, 3 / scale]);
      ctx.beginPath();
      live.forEach((p, i) => (i ? ctx.lineTo(p.x, p.y) : ctx.moveTo(p.x, p.y)));
      ctx.stroke();
      ctx.setLineDash([]);
      poly.forEach((p, i) => {
        ctx.beginPath();
        ctx.arc(p.x, p.y, (i === 0 ? 5 : 3.2) / scale, 0, Math.PI * 2);
        ctx.fillStyle = i === 0 ? '#ffa300' : '#fff';
        ctx.fill();
        ctx.lineWidth = 1.6 / scale;
        ctx.strokeStyle = INK;
        ctx.stroke();
      });
    }
    ctx.globalAlpha = 1;
    if (hover && !move && (opts.tool === 'pen' || opts.tool === 'oval' || opts.tool === 'box' || opts.tool === 'poly')) {
      ctx.beginPath();
      ctx.arc(hover.x, hover.y, 3 / scale, 0, Math.PI * 2);
      ctx.strokeStyle = INK;
      ctx.lineWidth = 1.4 / scale;
      ctx.stroke();
    }
    ctx.restore();
  }

  // ---- Public ------------------------------------------------------------

  function reflectSelection() {
    const shape = shapes()[selected];
    if (!shape) return;
    opts.role = shape.role;
    opts.paint = shape.paint;
    if (shape.kind !== 'ellipse') opts.smooth = shape.kind === 'rect' ? shape.radius > 0 : shape.smooth;
    if (slot === 'glasses') { opts.pair = shape.pair; opts.bridge = shape.bridge; }
    else opts.flip = shape.flip;
    onChange({ opts: { ...opts } });
  }

  function setOption(name, value) {
    opts[name] = value;
    if (name === 'tool') {
      draft = null; poly = null; move = null;
      if (value !== 'move') selected = -1;
      canvas.dataset.tool = value;
      say(HELP[value]);
    }
    const shape = shapes()[selected];
    if (shape && ['role', 'paint', 'smooth', 'pair', 'bridge', 'flip'].includes(name)) {
      const next = cloneShapes(shapes());
      const target = next[selected];
      if (name === 'role') target.role = value;
      else if (name === 'paint') target.paint = value;
      else if (name === 'smooth') { if (target.kind === 'rect') target.radius = value ? 1.5 : 0; else if (target.kind === 'poly') target.smooth = value; }
      else if (name === 'bridge') target.bridge = value && !target.pair;
      else if (name === 'flip') target.flip = value;
      else if (name === 'pair' && slot === 'glasses' && value !== target.pair) {
        translate(target, value ? -SEP : SEP, 0);
        target.pair = value;
        if (value) target.bridge = false;
      }
      replace(next);
    }
    onChange({ opts: { ...opts } });
    render();
  }

  function open(nextSlot) {
    slot = nextSlot;
    undoStack = [];
    redoStack = [];
    draft = null; poly = null; move = null; selected = -1;
    if (slot === 'glasses' && opts.paint === 'fill' && !shapes().length) opts.paint = 'glass';
    if (slot === 'hat' && opts.paint === 'glass') opts.paint = 'fill';
    canvas.dataset.tool = opts.tool;
    onChange({ opts: { ...opts }, slot });
    syncButtons();
    say(HELP[opts.tool]);
    render();
  }

  function close() {
    slot = null;
    draft = null; poly = null; move = null;
  }

  function applyTemplate(name) {
    const entry = TEMPLATES[slot].find(([label]) => label === name);
    if (!entry) return;
    const next = entry[1].map(parseShape).filter(Boolean);
    selected = -1;
    replace(next, `Started from the ${name.toLowerCase()}. Everything is editable.`);
  }

  function clear() {
    if (!shapes().length) return;
    selected = -1;
    replace([], 'Cleared the drawing.');
  }

  return { open, close, render, setOption, undo, redo, clear, applyTemplate, get slot() { return slot; }, get opts() { return opts; } };
}

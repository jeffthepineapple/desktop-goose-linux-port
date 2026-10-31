// Drawn items: the shape grammar shared with src/cosmetics.cpp.
//
//   <poly|ellipse|rect> <primary|accent> <fill|flat|line|glass> <flags|-> <args>
//
// poly: x,y points. ellipse: cx,cy rx,ry. rect: x,y w,h radius.
// flags: pair, bridge, flip, smooth, closed, w=<width>, comma separated.
// A shape object is {kind, role, paint, pair, bridge, flip, smooth, closed,
// width, radius, points: [[x, y], ...]}.

export const MAX_SHAPES = 32;
export const MAX_POINTS = 64;
export const MAX_COORD = 40;

const KINDS = { poly: 'poly', ellipse: 'ellipse', rect: 'rect' };
const ROLES = ['primary', 'accent'];
const PAINTS = ['fill', 'flat', 'line', 'glass'];
const FLAGS = ['pair', 'bridge', 'flip', 'smooth', 'closed'];

export function formatNumber(value) {
  const text = (Math.round(Number(value) * 100) / 100).toFixed(2).replace(/0+$/, '').replace(/\.$/, '');
  return text === '-0' ? '0' : text;
}

export function formatShape(shape) {
  const flags = FLAGS.filter((flag) => shape[flag]);
  if (shape.width > 0) flags.push(`w=${formatNumber(shape.width)}`);
  const parts = [shape.kind, shape.role, shape.paint, flags.length ? flags.join(',') : '-'];
  for (const point of shape.points || []) parts.push(`${formatNumber(point[0])},${formatNumber(point[1])}`);
  if (shape.kind === 'rect') parts.push(formatNumber(shape.radius || 0));
  return parts.join(' ');
}

const NUMBER = /^-?\d+(\.\d+)?$/;

function parsePoint(token) {
  const [x, y, extra] = token.split(',');
  if (extra !== undefined || !NUMBER.test(x ?? '') || !NUMBER.test(y ?? '')) return null;
  const point = [Number(x), Number(y)];
  return Math.abs(point[0]) <= MAX_COORD && Math.abs(point[1]) <= MAX_COORD ? point : null;
}

// Same acceptance rules as ParseShape in cosmetics.cpp. Returns null if the
// native app would drop the line.
export function parseShape(line) {
  const tokens = String(line).trim().split(/\s+/);
  if (tokens.length < 5) return null;
  const [kind, role, paint, flagText, ...args] = tokens;
  if (!KINDS[kind] || !ROLES.includes(role) || !PAINTS.includes(paint)) return null;

  const shape = {
    kind, role, paint,
    pair: false, bridge: false, flip: false, smooth: false, closed: false,
    width: 0, radius: 0, points: [],
  };
  if (flagText !== '-') {
    for (const flag of flagText.split(',')) {
      if (FLAGS.includes(flag)) {
        shape[flag] = true;
      } else if (flag.startsWith('w=') && NUMBER.test(flag.slice(2))) {
        shape.width = Number(flag.slice(2));
        if (shape.width < 0.1 || shape.width > 8) return null;
      } else {
        return null;
      }
    }
  }

  for (const token of args) {
    if (kind === 'rect' && shape.points.length === 2) {
      if (!NUMBER.test(token)) return null;
      shape.radius = Number(token);
      if (shape.radius < 0 || shape.radius > MAX_COORD) return null;
      continue;
    }
    const point = parsePoint(token);
    if (!point || shape.points.length >= MAX_POINTS) return null;
    shape.points.push(point);
  }

  if (kind === 'poly') {
    if (shape.points.length < (paint === 'line' ? 2 : 3)) return null;
  } else if (shape.points.length !== 2 || shape.points[1][0] <= 0 || shape.points[1][1] <= 0) {
    return null;
  }
  return shape;
}

// Anything that is not a valid shape (old storage, edited share link) is dropped.
export function sanitizeShapes(list) {
  if (!Array.isArray(list)) return [];
  const shapes = [];
  for (const entry of list) {
    const shape = parseShape(typeof entry === 'string' ? entry : safeFormat(entry));
    if (shape && shapes.length < MAX_SHAPES) shapes.push(shape);
  }
  return shapes;
}

function safeFormat(shape) {
  try { return shape && typeof shape === 'object' ? formatShape(shape) : ''; } catch { return ''; }
}

export const cloneShapes = (shapes) => shapes.map((shape) => ({ ...shape, points: shape.points.map((p) => [...p]) }));

// Ramer-Douglas-Peucker, to turn a freehand stroke into a few clean points.
export function simplify(points, epsilon) {
  if (points.length < 3) return points;
  const [ax, ay] = points[0];
  const [bx, by] = points[points.length - 1];
  const length = Math.hypot(bx - ax, by - ay);
  let index = 0;
  let farthest = 0;
  for (let i = 1; i < points.length - 1; i++) {
    const [px, py] = points[i];
    const distance = length === 0
      ? Math.hypot(px - ax, py - ay)
      : Math.abs((bx - ax) * (ay - py) - (ax - px) * (by - ay)) / length;
    if (distance > farthest) { farthest = distance; index = i; }
  }
  if (farthest <= epsilon) return [points[0], points[points.length - 1]];
  const left = simplify(points.slice(0, index + 1), epsilon);
  const right = simplify(points.slice(index), epsilon);
  return [...left.slice(0, -1), ...right];
}

// Starting points built from the same geometry as the built-in items.
export const TEMPLATES = {
  hat: [
    ['Cap', [
      'poly primary fill smooth -8,0 -7.5,-4.5 -4,-8 0,-9 4,-8 7.5,-4.5 8,0 0,0',
      'ellipse accent fill flip 7,0.5 7,2.45',
      'poly accent line w=1 0,-7 0,-1',
    ]],
    ['Top hat', [
      'rect primary fill - -7,-14 14,13 2',
      'rect accent flat - -7,-4 14,3 0',
      'rect primary fill - -11,-2 22,4 2',
    ]],
    ['Party hat', [
      'poly primary fill - -8,0 0,-18 8,0',
      'poly accent line w=2.2 -5,-5 5,-9',
      'ellipse accent flat - 0,-19 2.5,2.5',
    ]],
    ['Crown', [
      'poly primary fill - -9,1 -9,-10 -4,-5 0,-13 4,-5 9,-10 9,1',
      'ellipse accent flat - -5,-1.5 1.2,1.2',
      'ellipse accent flat - 0,-1.5 1.2,1.2',
      'ellipse accent flat - 5,-1.5 1.2,1.2',
    ]],
  ],
  glasses: [
    ['Round', [
      'ellipse accent glass pair 0,0 4.2,4.2',
      'poly primary line bridge,w=1.4 -1,0 1,0',
    ]],
    ['Shades', [
      'rect primary fill pair -3.75,-3.2 7.5,6.4 1.5',
      'poly primary line bridge,w=1.4 -1,0 1,0',
    ]],
    ['Aviators', [
      'ellipse accent glass pair 0,0 4.2,3.44',
      'poly primary line bridge,w=1.4 -1,0 1,0',
    ]],
  ],
};

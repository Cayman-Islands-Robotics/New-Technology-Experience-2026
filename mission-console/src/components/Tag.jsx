/**
 * Bracketed state code. `level` is semantic, not chromatic — the label alone
 * carries the state, so the tag stays readable in monochrome print and for
 * colour-blind operators.
 */
const CLASS = {
  ok: 'tag tag--ok',
  caution: 'tag tag--caution',
  alarm: 'tag tag--alarm',
  muted: 'tag tag--muted',
};

export function Tag({ level = 'muted', children }) {
  return <span className={CLASS[level] ?? CLASS.muted}>[{children}]</span>;
}

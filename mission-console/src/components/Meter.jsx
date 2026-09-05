/**
 * Proportion bar. Exposed as role="meter" with explicit bounds so the value is
 * available to assistive tech without reading the fill width.
 */
export function Meter({ value, min = 0, max = 100, label, alarm = false }) {
  const clamped = Math.max(min, Math.min(max, value));
  const width = ((clamped - min) / (max - min)) * 100;
  return (
    <div
      className="meter"
      role="meter"
      aria-label={label}
      aria-valuenow={Math.round(clamped)}
      aria-valuemin={min}
      aria-valuemax={max}
      aria-valuetext={`${Math.round(clamped)} percent`}
    >
      <div
        className={alarm ? 'meter__fill meter__fill--alarm' : 'meter__fill'}
        style={{ width: `${width}%` }}
      />
    </div>
  );
}

/** Single numeric readout. `breach` marks a value past its alarm threshold. */
export function Readout({ label, value, unit, note, aside, breach = false }) {
  return (
    <div className={breach ? 'readout readout--breach' : 'readout'}>
      <div className="readout__label">
        <span>{label}</span>
        {aside && <span>{aside}</span>}
      </div>
      <div className={breach ? 'readout__value readout__value--breach' : 'readout__value'}>
        {value}
        {unit && <span className="readout__unit">{unit}</span>}
      </div>
      {note && <div className="readout__note">{note}</div>}
    </div>
  );
}

/** Key/value block as a semantic <dl>. */
export function DefList({ rows }) {
  return (
    <dl className="deflist">
      {rows.map(([k, v]) => (
        <div className="deflist__row" key={k}>
          <dt>{k}</dt>
          <dd>{v}</dd>
        </div>
      ))}
    </dl>
  );
}

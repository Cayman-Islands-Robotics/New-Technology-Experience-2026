/**
 * Dense table renderer.
 *
 * columns: { key, label, numeric?, width?, render?(row), cellClass?(row) }
 * A visually hidden <caption> gives the table an accessible name; `rowKey`
 * keeps React reconciliation stable while rows stream in.
 */
export function DataTable({ caption, columns, rows, rowKey, rowClass }) {
  return (
    <div className="tablewrap">
      <table className="table">
        <caption className="visually-hidden">{caption}</caption>
        <thead>
          <tr>
            {columns.map((c) => (
              <th
                key={c.key}
                scope="col"
                className={c.numeric ? 'col--num' : undefined}
                style={c.width ? { width: c.width } : undefined}
              >
                {c.label}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map((row) => (
            <tr key={rowKey(row)} className={rowClass?.(row)}>
              {columns.map((c) => (
                <td
                  key={c.key}
                  className={[c.numeric ? 'col--num' : null, c.cellClass?.(row)]
                    .filter(Boolean)
                    .join(' ') || undefined}
                >
                  {c.render ? c.render(row) : row[c.key]}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

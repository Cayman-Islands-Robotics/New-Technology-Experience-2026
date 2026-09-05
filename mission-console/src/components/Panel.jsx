import { useId } from 'react';

/**
 * Titled region. Renders a real <section> labelled by its heading so the
 * panel is announced as a landmark rather than an anonymous box.
 */
export function Panel({ title, meta, flush = false, children, ...rest }) {
  const headingId = useId();
  return (
    <section className="panel" aria-labelledby={title ? headingId : undefined} {...rest}>
      {(title || meta) && (
        <header className="panel__head">
          {title && (
            <h2 className="panel__title" id={headingId}>
              {title}
            </h2>
          )}
          {meta && <span className="panel__meta">{meta}</span>}
        </header>
      )}
      <div className={flush ? 'panel__body panel__body--flush' : 'panel__body'}>{children}</div>
    </section>
  );
}

import { useRef } from 'react';

/**
 * WAI-ARIA tab pattern with a roving tabindex: exactly one tab is in the tab
 * order, and Left/Right/Home/End move selection between them. Manual DOM focus
 * is required because selection and focus must move together.
 */
export function TabNav({ tabs, active, onChange, label }) {
  const stripRef = useRef(null);

  function onKeyDown(event) {
    const keys = ['ArrowRight', 'ArrowLeft', 'Home', 'End'];
    if (!keys.includes(event.key)) return;
    event.preventDefault();

    const index = tabs.findIndex((t) => t.id === active);
    let next = index;
    if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
    if (event.key === 'ArrowLeft') next = (index - 1 + tabs.length) % tabs.length;
    if (event.key === 'Home') next = 0;
    if (event.key === 'End') next = tabs.length - 1;

    onChange(tabs[next].id);
    stripRef.current?.querySelectorAll('[role="tab"]')[next]?.focus();
  }

  return (
    <div className="tabstrip" role="tablist" aria-label={label} ref={stripRef} onKeyDown={onKeyDown}>
      {tabs.map((t) => {
        const selected = t.id === active;
        return (
          <button
            key={t.id}
            type="button"
            role="tab"
            id={`tab-${t.id}`}
            className="tabstrip__tab"
            aria-selected={selected}
            aria-controls={`panel-${t.id}`}
            tabIndex={selected ? 0 : -1}
            onClick={() => onChange(t.id)}
          >
            {t.label}
            {t.count != null && (
              <span className="tabstrip__count">
                {t.count}
                <span className="visually-hidden"> items</span>
              </span>
            )}
          </button>
        );
      })}
    </div>
  );
}

/**
 * Panel half of the tab pattern. Inactive panels unmount rather than hide, so
 * their contents stay out of the accessibility tree and the tab order.
 */
export function TabPanel({ id, active, children }) {
  if (!active) return null;
  return (
    <div role="tabpanel" id={`panel-${id}`} aria-labelledby={`tab-${id}`} tabIndex={0}>
      {children}
    </div>
  );
}

import { useEffect, useLayoutEffect, useRef, useState } from 'react';

/* Measuring must happen before paint so the fallback width is never shown, but
   useLayoutEffect warns under server rendering. Fall back to useEffect there. */
const useIsomorphicLayoutEffect = typeof window !== 'undefined' ? useLayoutEffect : useEffect;

/**
 * Measures a element's content-box width via ResizeObserver.
 *
 * Needed by any SVG that must render at 1:1 device pixels: scaling a fixed
 * viewBox to fit stretches the glyphs, and `non-scaling-stroke` only rescues
 * the strokes. Measuring lets the chart lay itself out in real CSS pixels.
 */
export function useElementWidth(fallback = 640) {
  const ref = useRef(null);
  const [width, setWidth] = useState(0);

  useIsomorphicLayoutEffect(() => {
    const el = ref.current;
    if (!el) return undefined;

    // No ResizeObserver under SSR or older engines — take one static measure.
    if (typeof ResizeObserver === 'undefined') {
      setWidth(el.clientWidth || fallback);
      return undefined;
    }

    const ro = new ResizeObserver(([entry]) => {
      setWidth(Math.round(entry.contentRect.width));
    });
    ro.observe(el);
    setWidth(el.clientWidth);
    return () => ro.disconnect();
  }, [fallback]);

  return [ref, width || fallback];
}

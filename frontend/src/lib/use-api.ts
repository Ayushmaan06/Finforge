"use client";

import { useCallback, useEffect, useRef, useState } from "react";

type State<T> = { data?: T; error?: Error; settled?: string };

/**
 * Load data for `key` (refetched when the key changes or on `reload()`). The loader may close over
 * anything; only `key` decides when to fetch again. State is only set from async callbacks.
 */
export function useApi<T>(load: () => Promise<T>, key = "") {
  const loader = useRef(load);
  useEffect(() => {
    loader.current = load;
  });
  const [nonce, setNonce] = useState(0);
  const [state, setState] = useState<State<T>>({});
  const token = `${key}#${nonce}`;

  useEffect(() => {
    let live = true;
    loader.current().then(
      (data) => live && setState({ data, settled: token }),
      (error: Error) => live && setState((s) => ({ data: s.data, error, settled: token })),
    );
    return () => {
      live = false;
    };
  }, [token]);

  const reload = useCallback(() => setNonce((n) => n + 1), []);
  return { data: state.data, error: state.error, loading: state.settled !== token, reload };
}

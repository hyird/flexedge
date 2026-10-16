import { useState, type SetStateAction } from 'react'
import { useLocation, type HistoryState } from '@tanstack/react-router'

/** Arrival requests carried in history state, never in the URL: a link that
 * opens a page on a specific in-page tab (and that tab's starting values)
 * passes them as `state` (`navigate({ to, state })`, `<Link state>`, or
 * `state` on `LinkButton`). Values read back after a
 * reload may be stale; pages fall back to their defaults for unknown ones. */
declare module '@tanstack/history' {
  interface HistoryState {
    /** `/websites/new`. */
    website?: {
      /** Settings group (`websiteSectionIds`). */
      section?: string
    }
  }
}

/** Local state seeded from the navigation that opened the page. Each new
 * navigation to the page (a new history entry, even with the same URL)
 * re-seeds it; in between it is plain component state that never touches the
 * URL. Returns the value, its setter and the arrival key (use it as a React
 * `key` to remount tab content that took its starting values from it). */
export function useArrivalState<T>(seed: (state: HistoryState) => T) {
  const state = useLocation({ select: (location) => location.state })
  const key = state.__TSR_key ?? state.key ?? ''
  const [current, setCurrent] = useState(() => ({ key, value: seed(state) }))
  let active = current
  if (current.key !== key) {
    active = { key, value: seed(state) }
    setCurrent(active)
  }
  const setValue = (update: SetStateAction<T>) =>
    setCurrent((previous) => ({
      key: previous.key,
      value:
        typeof update === 'function'
          ? (update as (value: T) => T)(previous.value)
          : update,
    }))
  return [active.value, setValue, active.key] as const
}

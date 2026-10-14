import { useSyncExternalStore } from 'react'

// One shared display clock for every mounted relative time. It only re-renders
// labels such as "3 分钟前"; it never reads data.
const clockListeners = new Set<() => void>()
let clockTimer: number | undefined
let clockNow = Date.now()
function subscribeClock(listener: () => void) {
  clockListeners.add(listener)
  if (clockTimer === undefined)
    clockTimer = window.setInterval(() => {
      clockNow = Date.now()
      clockListeners.forEach((notify) => notify())
    }, 30_000)
  return () => {
    clockListeners.delete(listener)
    if (clockListeners.size === 0) {
      window.clearInterval(clockTimer)
      clockTimer = undefined
    }
  }
}

/** Current time from the shared 30 s display clock, for relative labels. */
export function useDisplayNow() {
  return useSyncExternalStore(subscribeClock, () => clockNow)
}

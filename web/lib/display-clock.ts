import { useSyncExternalStore } from 'react'

// Shared display clocks for every mounted relative time, countdown and
// elapsed time. They only re-render labels such as "3 分钟前" or "12 秒后重试";
// they never read data. Each ticks only while something is subscribed.
function createDisplayClock(intervalMs: number) {
  const listeners = new Set<() => void>()
  let timer: number | undefined
  let now = Date.now()
  const subscribe = (listener: () => void) => {
    listeners.add(listener)
    if (timer === undefined) {
      now = Date.now()
      timer = window.setInterval(() => {
        now = Date.now()
        listeners.forEach((notify) => notify())
      }, intervalMs)
    }
    return () => {
      listeners.delete(listener)
      if (listeners.size === 0) {
        window.clearInterval(timer)
        timer = undefined
      }
    }
  }
  return { subscribe, read: () => now }
}

const halfMinuteClock = createDisplayClock(30_000)
const secondClock = createDisplayClock(1000)
/** Current time from the shared 30 s display clock, for relative labels. */
export function useDisplayNow() {
  return useSyncExternalStore(
    halfMinuteClock.subscribe,
    halfMinuteClock.read,
    halfMinuteClock.read
  )
}

/** Current time from the shared 1 s display clock, for second-level
 * countdowns (`12 秒后重试`) and live elapsed times of running work. */
export function useDisplaySeconds() {
  return useSyncExternalStore(
    secondClock.subscribe,
    secondClock.read,
    secondClock.read
  )
}

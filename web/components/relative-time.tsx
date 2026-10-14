import { useSyncExternalStore } from 'react'
import { Tooltip } from '@heroui/react'
import { formatRelative, formatUtcDate } from '@/lib/format'

// One shared display clock for all mounted relative times. It only re-renders
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

/** Relative time (`3 分钟前`) with the absolute UTC time in a tooltip. */
export function RelativeTime({
  value,
  className,
}: {
  value?: string
  className?: string
}) {
  const now = useSyncExternalStore(subscribeClock, () => clockNow)
  if (!value) return <span className={className}>—</span>
  return (
    <Tooltip delay={300}>
      <Tooltip.Trigger aria-label={formatUtcDate(value)}>
        <time dateTime={value} className={className}>
          {formatRelative(value, now)}
        </time>
      </Tooltip.Trigger>
      <Tooltip.Content>{formatUtcDate(value)}</Tooltip.Content>
    </Tooltip>
  )
}

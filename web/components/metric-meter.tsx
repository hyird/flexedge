import { Meter } from '@heroui/react'
import { formatPercent } from '@/lib/format'
import { cn } from '@/lib/utils'

/** Compact utilization bar with its percentage, for CPU/memory and similar
 * ratios in tables. `value` is a ratio in [0, 1]; missing data shows `—`.
 * Color escalates at 75% (warning) and 90% (danger). */
export function MetricMeter({
  label,
  value,
  className,
}: {
  /** Accessible name, e.g. `CPU`. */
  label: string
  value?: number
  className?: string
}) {
  if (value === undefined || !Number.isFinite(value))
    return <span className='text-xs text-muted'>—</span>
  const ratio = Math.min(Math.max(value, 0), 1)
  return (
    <Meter
      aria-label={label}
      value={ratio * 100}
      size='sm'
      color={ratio >= 0.9 ? 'danger' : ratio >= 0.75 ? 'warning' : 'accent'}
      className={cn('flex w-28 flex-row items-center gap-2', className)}
    >
      <Meter.Track className='flex-1'>
        <Meter.Fill />
      </Meter.Track>
      <span className='w-11 shrink-0 text-right text-xs text-muted tabular-nums'>
        {formatPercent(ratio, 0)}
      </span>
    </Meter>
  )
}

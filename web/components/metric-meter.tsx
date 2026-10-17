import { Meter } from '@heroui/react'
import { cn } from '@/lib/utils'

/** CPU/memory style utilization in tables: a small HeroUI Meter with its
 * percentage output. `value` is a ratio in [0, 1]; missing data keeps the same
 * shape with an empty track and `—`. Color escalates at 75% (warning) and 90%
 * (danger). */
export function MetricMeter({
  label,
  value,
  capacity,
  className,
}: {
  /** Accessible name, e.g. `CPU`. */
  label: string
  value?: number
  /** Total CPU cores or memory capacity, shown beside the percentage. */
  capacity?: string
  className?: string
}) {
  const ratio =
    value === undefined || !Number.isFinite(value)
      ? undefined
      : Math.min(Math.max(value, 0), 1)
  return (
    <Meter
      aria-label={label}
      value={(ratio ?? 0) * 100}
      valueLabel={ratio === undefined ? '—' : undefined}
      size='sm'
      color={
        ratio === undefined
          ? 'default'
          : ratio >= 0.9
            ? 'danger'
            : ratio >= 0.75
              ? 'warning'
              : 'accent'
      }
      className={cn('w-28 gap-x-2 gap-y-1 tabular-nums', className)}
    >
      <Meter.Output className='col-span-1 col-start-1 row-start-1 justify-self-start text-xs font-normal text-muted' />
      {capacity !== undefined && (
        <span className='col-start-2 row-start-1 justify-self-end text-xs whitespace-nowrap text-muted'>
          {capacity}
        </span>
      )}
      <Meter.Track>
        <Meter.Fill />
      </Meter.Track>
    </Meter>
  )
}

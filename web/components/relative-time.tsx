import { Tooltip } from '@heroui/react'
import { useDisplayNow } from '@/lib/display-clock'
import { formatRelative, formatUtcDate } from '@/lib/format'

/** Relative time (`3 分钟前`) with the absolute UTC time in a tooltip. */
export function RelativeTime({
  value,
  className,
}: {
  value?: string
  className?: string
}) {
  const now = useDisplayNow()
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

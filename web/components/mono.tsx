import { type ReactNode } from 'react'
import { cn } from '@/lib/utils'

/** Monospace 13px text for domains, IPs, IDs, paths and ports. `truncate`
 * keeps long values on one line with the full value in the title. */
export function Mono({
  children,
  truncate = false,
  className,
}: {
  children: ReactNode
  truncate?: boolean
  className?: string
}) {
  return (
    <span
      title={truncate && typeof children === 'string' ? children : undefined}
      className={cn(
        'font-mono text-[13px] tabular-nums',
        truncate && 'block max-w-full truncate',
        className
      )}
    >
      {children}
    </span>
  )
}

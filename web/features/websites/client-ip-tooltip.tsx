import type { ReactNode } from 'react'
import { Tooltip } from '@heroui/react'
import { formatClientLocation } from './client-location'

/** AS details and optional geography appear on hover or keyboard focus. */
export function ClientIpTooltip({
  asn,
  asName,
  location,
  label,
  children,
  className,
}: {
  asn?: string
  asName?: string
  location?: string
  label: string
  children?: ReactNode
  className?: string
}) {
  const details = [asn?.trim(), asName?.trim()].filter(Boolean).join(' ')
  if (!location && !details) {
    return <span className={className}>{children ?? label}</span>
  }
  return (
    <Tooltip delay={300}>
      <Tooltip.Trigger aria-label={label} className={className}>
        {children ?? label}
      </Tooltip.Trigger>
      <Tooltip.Content className='max-w-80 break-normal wrap-break-word'>
        {location && <div>{formatClientLocation(location)}</div>}
        {details && <div>{details}</div>}
      </Tooltip.Content>
    </Tooltip>
  )
}

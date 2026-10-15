import { Tooltip } from '@heroui/react'
import { Mono } from '@/components/mono'
import { accessLogAsnLabel } from './access-log-filter'
import type { AccessLog } from './access-log-schema'

type AccessLogAsnFields = Pick<AccessLog, 'client_ip_asn' | 'client_ip_as_name'>

function AsnText({
  log,
  className,
}: {
  log: AccessLogAsnFields
  className?: string
}) {
  return (
    <>
      {log.client_ip_asn && (
        <Mono className={className}>{log.client_ip_asn}</Mono>
      )}
      {log.client_ip_asn && log.client_ip_as_name && ' '}
      {log.client_ip_as_name}
    </>
  )
}

/** Second line of the 客户端 cell: `台湾 · AS4134 Chinanet`, one line
 * truncated within the column's original width, full text in the tooltip. */
export function AccessLogClientOrigin({
  log,
}: {
  log: AccessLogAsnFields & Pick<AccessLog, 'client_ip_location'>
}) {
  const asn = accessLogAsnLabel(log)
  const label = [log.client_ip_location, asn].filter(Boolean).join(' · ')
  if (!label) return <span className='text-xs text-muted'>—</span>
  return (
    <Tooltip delay={300}>
      <Tooltip.Trigger
        aria-label={label}
        className='block max-w-32 truncate text-xs text-muted'
      >
        {log.client_ip_location}
        {log.client_ip_location && asn && ' · '}
        <AsnText log={log} className='text-xs' />
      </Tooltip.Trigger>
      <Tooltip.Content className='max-w-80'>{label}</Tooltip.Content>
    </Tooltip>
  )
}

/** Request detail value: the full ASN and AS name, wrapping when long. */
export function AccessLogAsnDetail({ log }: { log: AccessLogAsnFields }) {
  if (!accessLogAsnLabel(log)) return '—'
  return (
    <span className='break-words'>
      <AsnText log={log} />
    </span>
  )
}

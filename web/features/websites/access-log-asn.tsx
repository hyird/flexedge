import { Tooltip } from '@heroui/react'
import { Mono } from '@/components/mono'
import { accessLogAsnLabel } from './access-log-filter'
import type { AccessLog } from './access-log-schema'

type AccessLogAsnFields = Pick<AccessLog, 'client_ip_asn' | 'client_ip_as_name'>

function AsnText({ log }: { log: AccessLogAsnFields }) {
  return (
    <>
      {log.client_ip_asn && <Mono>{log.client_ip_asn}</Mono>}
      {log.client_ip_asn && log.client_ip_as_name && ' '}
      {log.client_ip_as_name}
    </>
  )
}

/** ASN column cell: `AS4134 Chinanet` on one line, truncated at the column
 * width with the full text in the tooltip; a dash without AS data. */
export function AccessLogAsnCell({ log }: { log: AccessLogAsnFields }) {
  const label = accessLogAsnLabel(log)
  if (!label) return <span className='text-muted'>—</span>
  return (
    <Tooltip delay={300}>
      <Tooltip.Trigger
        aria-label={label}
        className='block w-0 min-w-full truncate'
      >
        <AsnText log={log} />
      </Tooltip.Trigger>
      <Tooltip.Content className='max-w-80 break-normal wrap-break-word'>
        {label}
      </Tooltip.Content>
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

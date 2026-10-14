import { StatusChip } from '@/components/status-chip'
import { registrationLabels } from './node-status'

/** Registration state (`pending` / `registered`); `pending` reads “等待接入”. */
export function RegistrationChip({ status }: { status: string }) {
  return (
    <StatusChip
      status={status}
      label={status === 'pending' ? '等待接入' : undefined}
      reserve={registrationLabels}
    />
  )
}

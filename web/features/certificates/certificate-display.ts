import { ApiBusinessError } from '@/lib/api-response'
import type { StatusTone } from '@/lib/status'
import type { Certificate } from './types'

export const certificateStatuses = [
  { id: 'pending', label: '等待签发' },
  { id: 'issuing', label: '签发中' },
  { id: 'valid', label: '有效' },
  { id: 'renewing', label: '续期中' },
  { id: 'expired', label: '已过期' },
  { id: 'failed', label: '失败' },
] as const
export const certificateStatusIds = certificateStatuses.map(({ id }) => id) as [
  (typeof certificateStatuses)[number]['id'],
  ...(typeof certificateStatuses)[number]['id'][],
]

/** Days before expiry at which a valid certificate counts as "即将到期",
 * matching the overview's `certificate_expiring_count`. */
const expiringWithinDays = 30

/** Valid and expiring within 30 days: highlights the remaining days in the
 * list, same rule as the server's `expiring=1` filter. */
export function isExpiring(certificate: Certificate) {
  return (
    certificate.status === 'valid' &&
    certificate.remaining_days !== undefined &&
    certificate.remaining_days <= expiringWithinDays
  )
}

const dayMs = 86_400_000

/** Remaining share of the validity window, for the detail progress bar. */
export function validityProgress(certificate: Certificate) {
  const start = Date.parse(certificate.not_before ?? '')
  const end = Date.parse(certificate.expires_at ?? '')
  if (Number.isNaN(start) || Number.isNaN(end) || end <= start) return
  const totalDays = Math.round((end - start) / dayMs)
  const remainingDays =
    certificate.remaining_days ??
    Math.max(0, Math.floor((end - Date.now()) / dayMs))
  const tone: StatusTone =
    remainingDays <= 7
      ? 'danger'
      : remainingDays <= expiringWithinDays
        ? 'warning'
        : 'success'
  return {
    totalDays,
    remainingDays,
    percent: Math.min(100, Math.max(0, (remainingDays / totalDays) * 100)),
    tone,
  }
}

/** `aa:bb:…` form of a hex SHA-256 fingerprint for display. */
export function formatFingerprint(value: string) {
  return /^[0-9a-f]+$/i.test(value)
    ? (value.toUpperCase().match(/.{1,2}/g) ?? []).join(':')
    : value
}

const certificateNotFoundCode = 16001

export function isCertificateNotFound(error: unknown) {
  return (
    error instanceof ApiBusinessError && error.code === certificateNotFoundCode
  )
}

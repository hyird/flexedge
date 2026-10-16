import { statusReserve, type StatusDictionary } from '@/lib/status'

export function providerLabel(provider: string) {
  if (provider === 'cloudflare') return 'Cloudflare'
  if (provider === 'aliyun') return '阿里云'
  if (provider === 'letsencrypt') return "Let's Encrypt"
  if (provider === 'zerossl') return 'ZeroSSL'
  return provider
}

/** A provider account's credential check: unverified after create or a
 * credential change, then verified or invalid. */
export const providerStatuses = {
  unverified: { label: '未验证', tone: 'warning' },
  verified: { label: '已验证', tone: 'success' },
  invalid: { label: '无效', tone: 'danger' },
} as const satisfies StatusDictionary<'unverified' | 'verified' | 'invalid'>

export const providerStatusReserve = statusReserve(providerStatuses)

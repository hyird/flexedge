import { statusLabels } from '@/lib/status'

export function providerLabel(provider: string) {
  if (provider === 'cloudflare') return 'Cloudflare'
  if (provider === 'aliyun') return '阿里云'
  if (provider === 'letsencrypt') return "Let's Encrypt"
  if (provider === 'zerossl') return 'ZeroSSL'
  return provider
}

/** Every label a provider account's `status` can show (unverified after
 * create/credential change, then verified or invalid), for `reserve`. */
export const providerStatusLabels = statusLabels([
  'unverified',
  'verified',
  'invalid',
])

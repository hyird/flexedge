import type { Revisioned } from '@/lib/resource'

export type DnsProvider = Revisioned & {
  name: string
  account_id: string
  provider: string
  token_hint: string
  status: string
  zone_count: number
  last_verified_at?: string
  last_error?: string
}

export type CertificateProvider = Revisioned & {
  provider: string
  credential_mode: string
  account_email?: string
  access_key_hint?: string
  status: string
  last_verified_at?: string
  last_error?: string
}

export type DnsProviderInput = {
  name: string
  provider: 'cloudflare' | 'aliyun'
  account_id: string
  api_token: string
}
export type CertificateProviderInput = {
  provider: 'letsencrypt' | 'zerossl'
  credential_mode: 'email' | 'access_key'
  account_email: string
  access_key: string
}
export type ProviderTarget = {
  kind: 'dns' | 'certificate'
  id: string
  revision: number
}

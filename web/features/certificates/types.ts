import type { Revisioned } from '@/lib/resource'

export type Certificate = Revisioned & {
  domains: string[]
  issuer: string
  certificate_provider_id: string
  certificate_provider: string
  status: string
  usable: boolean
  config: { auto_renew: boolean }
  dns_zone_id: string
  dns_zone_domain: string
  not_before?: string
  expires_at?: string
  remaining_days?: number
  last_error?: string
  serial_number?: string
  fingerprint_sha256?: string
  last_issued_at?: string
  sync_status?: string
  sync_count_fails?: number
  website_count: number
}

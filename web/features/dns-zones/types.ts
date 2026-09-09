import type { Revisioned } from '@/lib/resource'

export type DnsLine = {
  code: string
  name: string
  display_name: string
  status: string
}

export type DnsRecord = {
  id: string
  type: 'A' | 'AAAA' | 'CNAME' | 'TXT' | 'MX'
  name: string
  content: string
  ttl: number
  priority?: number
  proxied: boolean
  line_code: string
}

export type DnsZone = Revisioned & {
  dns_provider_id: string
  dns_provider: string
  dns_provider_name: string
  domain: string
  sync_status: string
  desired_revision: number
  synced_revision: number
  website_count: number
  config: { records: DnsRecord[] }
  runtime: {
    records_imported: boolean
    lines_synced_at?: string
    lines: DnsLine[]
    projected_records: DnsRecord[]
    record_states: Array<{
      id: string
      sync_status: string
      synced_revision: number
      last_error?: string
    }>
    conflicts: Array<{
      id: string
      type: string
      name: string
      line_code: string
      local_content: string
      remote_content: string
    }>
  }
  last_synced_at?: string
  last_error?: string
}

export type DnsZoneOption = {
  id: string
  domain: string
  dns_provider: string
  dns_provider_name: string
  sync_status: string
  available: boolean
}

import type { Revisioned } from '@/lib/resource'

export type Cluster = Revisioned & {
  name: string
  dns_zone_id: string
  dns_zone_domain: string
  dns_provider_name: string
  hostname_prefix: string
  access_domain: string
  node_count: number
  online_node_count: number
  status: string
}

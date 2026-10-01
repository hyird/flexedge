import type { Revisioned } from '@/lib/resource'

export type NodeEndpoint = {
  id: string
  ip_address: string
  line_code: string
}

export type Node = Revisioned & {
  cluster_id: string
  cluster_name: string
  name: string
  status: string
  node_spec_revision: number
  config: { endpoints: NodeEndpoint[]; cache?: NodeCacheConfig }
  runtime: {
    cache_statistics?: {
      memory_hits: number
      disk_hits: number
      misses: number
      bypasses: number
      stale_hits: number
      revalidations: number
      origin_bytes: number
      cache_bytes: number
      coalesced: number
      active_waiters: number
      memory_bytes: number
      disk_bytes: number
      entries: number
      writes: number
      write_errors: number
      evictions: number
    } | null
    registration_status: string
    connection_status: string
    last_heartbeat_at?: string
    applied_node_spec_revision: number
    active_release_id?: string
    active_manifest_digest?: string
    agent_version?: string
    cpu_usage?: number
    memory_usage?: number
    traffic_out_bps?: number
    connection_count?: number
    load_1m?: number
    queued_log_events?: number
    dropped_log_events?: number
    health?: string
    last_error?: string
  }
}
export type NodeCacheConfig = {
  memory_bytes: number
  disk_bytes: number
  max_object_bytes: number
  directory: string
}

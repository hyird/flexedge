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
  config: { endpoints: NodeEndpoint[] }
  runtime: {
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

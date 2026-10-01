export type CacheJobResult = {
  target: string
  status: 'completed' | 'failed' | 'cancelled'
  error?: string
  bytes?: number
}
export type CacheJobNode = {
  node_id: string
  node_name: string
  command_id?: string
  status: string
  error?: string
  started_at?: string
  completed_at?: string
  results: CacheJobResult[]
}
export type CacheJob = {
  id: string
  website_id: string
  website_revision: number
  policy_revision: number
  operation: 'purge' | 'preheat'
  mode: 'all' | 'url' | 'prefix'
  targets: string[]
  concurrency: number
  rate_limit_bps: number
  status: string
  revision: number
  error?: string
  created_at: string
  updated_at: string
  nodes: CacheJobNode[]
}

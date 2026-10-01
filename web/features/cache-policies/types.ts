export type CacheRule = {
  id: string
  name: string
  status: 'enabled' | 'disabled'
  match_type: 'all' | 'prefix' | 'exact' | 'extension'
  patterns: string[]
  action: 'cache' | 'bypass'
  ttl_seconds: number
  query_mode: 'include' | 'ignore'
  status_codes: number[]
  min_object_bytes: number
  max_object_bytes: number
  stale_if_error_seconds: number
  ignore_origin_cache_control: boolean
  range_enabled: boolean
}
export type CachePolicy = {
  id: string
  name: string
  description: string
  status: 'enabled' | 'disabled'
  revision: number
  rules: CacheRule[]
  website_count: number
  websites: Array<{ id: string; name: string; status: string }>
  created_at: string
  updated_at: string
}

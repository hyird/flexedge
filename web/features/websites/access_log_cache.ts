import type { AccessLog } from './access-log-schema'

const cacheStates = {
  hit_memory: { label: 'HIT', tone: 'success' },
  hit_disk: { label: 'HIT', tone: 'success' },
  revalidated: { label: 'REVALIDATED', tone: 'success' },
  miss: { label: 'MISS', tone: 'warning' },
  bypass: { label: 'BYPASS', tone: 'default' },
} as const

const reasons: Record<string, string> = {
  not_evaluated: '请求未进入缓存处理',
  fresh: '缓存仍在有效期内',
  cache_disabled: '网站未启用缓存',
  policy_disabled: '缓存策略已停用',
  no_rule: '没有匹配的缓存规则',
  rule_bypass: '命中不缓存规则',
  request_method: '请求方法不支持缓存',
  request_authorization: '请求携带身份认证',
  request_cookie: '请求携带Cookie',
  request_upgrade: '请求升级协议',
  request_event_stream: '请求事件流',
  request_no_store: '请求指定no-store',
  request_no_cache: '浏览器要求刷新，按规则直接回源',
  request_target_too_long: '请求地址超过缓存键长度限制',
  request_url: '请求地址无法作为缓存键',
  range_unsupported: '不支持此范围请求',
  range_not_cached: '范围请求没有可用的完整缓存',
  cache_empty: '节点尚无可用缓存',
  cache_expired: '缓存已过期，需要回源',
  cache_busy: '等待其他请求填充缓存超时',
  origin_private: '源站指定private，禁止共享缓存',
  origin_no_store: '源站指定no-store，禁止缓存',
  origin_no_cache: '源站指定no-cache，规则要求跳过',
  origin_set_cookie: '源站携带Set-Cookie，规则要求跳过',
  origin_status: '源站状态码不在缓存范围内',
  origin_event_stream: '源站返回事件流',
  origin_upgrade: '源站升级协议',
  origin_vary: '源站Vary不支持缓存',
  origin_ttl: '源站未提供有效缓存时间，或有效期已耗尽',
  origin_changed: '源站返回完整响应，更新缓存',
  origin_not_modified: '源站返回304，复用已有缓存',
  origin_error_stale: '源站失败，使用允许的过期缓存',
  object_too_large: '响应超过单对象缓存上限',
  object_too_small: '响应小于规则规定的缓存下限',
  memory_limit: '可用内存不足以缓存此响应',
  cache_write_failed: '缓存未能写入或发布',
}

export function cacheState(log: Pick<AccessLog, 'cache_status'>) {
  return cacheStates[log.cache_status as keyof typeof cacheStates]
}

export function cacheReason(
  log: Pick<AccessLog, 'cache_status' | 'cache_reason'>
) {
  const tier =
    log.cache_status === 'hit_memory'
      ? 'L1内存命中；'
      : log.cache_status === 'hit_disk'
        ? 'L2磁盘命中；'
        : ''
  return (
    tier +
    (reasons[log.cache_reason ?? ''] || log.cache_reason || '节点未上报原因')
  )
}

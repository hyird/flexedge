import { describe, expect, it } from 'vitest'
import { cacheReason, cacheState } from './access_log_cache'

describe('access log cache outcome', () => {
  it('uses four compact request labels and keeps tiers in the explanation', () => {
    expect(cacheState({ cache_status: 'hit_memory' })?.label).toBe('HIT')
    expect(cacheState({ cache_status: 'hit_disk' })?.label).toBe('HIT')
    expect(cacheState({ cache_status: 'miss' })?.label).toBe('MISS')
    expect(cacheState({ cache_status: 'bypass' })?.label).toBe('BYPASS')
    expect(cacheState({ cache_status: 'revalidated' })?.label).toBe(
      'REVALIDATED'
    )
    expect(
      cacheReason({ cache_status: 'hit_disk', cache_reason: 'fresh' })
    ).toContain('L2磁盘')
  })
  it('explains refresh, origin restrictions and validation independently', () => {
    expect(cacheReason({ cache_reason: 'request_no_cache' })).toContain(
      '浏览器要求刷新'
    )
    expect(cacheReason({ cache_reason: 'origin_no_cache' })).toContain(
      '源站指定no-cache'
    )
    expect(cacheReason({ cache_reason: 'origin_not_modified' })).toContain(
      '304'
    )
    expect(cacheState({})).toBeUndefined()
    expect(cacheState({ cache_status: 'future' })).toBeUndefined()
  })
})

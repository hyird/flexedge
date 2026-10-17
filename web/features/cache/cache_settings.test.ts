import { describe, expect, it } from 'vitest'
import {
  cacheSettingsSchema,
  newCacheSettings,
  removeCacheSetting,
} from './cache_settings'

describe('optional cache settings', () => {
  it('round trips explicit false and zero TTL through JSON', () => {
    const settings = newCacheSettings()
    settings.enabled = ['edge', 'status', 'stale', 'origin']
    settings.status_ttl = [
      { from: 200, to: 299, seconds: 0 },
      { from: 500, to: 999, seconds: -1 },
    ]
    settings.serve_stale = false
    settings.origin_control = false
    expect(
      cacheSettingsSchema.parse(JSON.parse(JSON.stringify(settings)))
    ).toEqual(settings)
  })
  it('rejects overlapping status ranges and missing query parameters', () => {
    const settings = newCacheSettings()
    settings.status_ttl = [
      { from: 200, to: 299, seconds: 100 },
      { from: 250, to: 404, seconds: 0 },
    ]
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(false)
    settings.status_ttl = []
    settings.key.query_mode = 'include'
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(false)
  })
  it('removes incomplete settings without blocking unrelated saves', () => {
    const settings = newCacheSettings()
    settings.enabled = ['key', 'vary', 'edge']
    settings.key.query_mode = 'include'
    settings.vary_headers = [{ name: '', action: 'passthrough', values: [] }]
    const removed = removeCacheSetting(
      removeCacheSetting(settings, 'key'),
      'vary'
    )
    expect(cacheSettingsSchema.safeParse(removed).success).toBe(true)
    expect(removed.enabled).toEqual(['edge'])
  })
  it('rejects unsafe Vary configurations', () => {
    const settings = newCacheSettings()
    settings.vary_headers = [
      { name: 'cf-country', action: 'passthrough', values: [] },
    ]
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(false)
    settings.vary_headers = [
      { name: 'x-custom', action: 'normalize', values: [] },
    ]
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(true)
    settings.vary_headers = [
      { name: 'accept-language', action: 'normalize', values: ['中文'] },
    ]
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(false)
  })
  it('requires selected values for restricted key headers while allowing presence checks', () => {
    const settings = newCacheSettings()
    settings.key.headers = ['accept-language']
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(false)
    settings.key.headers = []
    settings.key.header_presence = ['accept-language']
    settings.key.header_values = [
      { name: 'accept-language', values: ['zh', 'en'] },
    ]
    expect(cacheSettingsSchema.safeParse(settings).success).toBe(true)
  })
})

import { describe, it, expect } from 'vitest'
import {
  websiteFormSchema,
  defaultWebsiteConfig,
} from '@/features/websites/website-form'
import {
  cacheConfigSchema,
  cacheOperationInputSchema,
  defaultCacheConfig,
  newCacheRule,
} from './types'

describe('cache policy boundaries', () => {
  it('rejects unsafe disk paths, invalid regex and conflicting size limits', () => {
    expect(
      cacheConfigSchema.safeParse({
        ...defaultCacheConfig(),
        directory: '../cache',
      }).success
    ).toBe(false)
    expect(
      cacheConfigSchema.safeParse({
        ...defaultCacheConfig(),
        disk_percent: 0,
        disk_bytes: 1,
      }).success
    ).toBe(false)
    const rule = {
      ...newCacheRule(),
      name: 'rule',
      match_type: 'regex',
      patterns: ['('],
    }
    expect(
      cacheConfigSchema.safeParse({ ...defaultCacheConfig(), rules: [rule] })
        .success
    ).toBe(false)
    expect(
      cacheConfigSchema.safeParse({
        ...defaultCacheConfig(),
        rules: [
          {
            ...rule,
            match_type: 'all',
            patterns: [],
            min_object_bytes: 20,
            max_object_bytes: 10,
          },
        ],
      }).success
    ).toBe(false)
  })
  it('uses percentage budgets and independent editable default rules', () => {
    const first = defaultCacheConfig(),
      second = defaultCacheConfig()
    expect(first.memory_percent).toBe(10)
    expect(first.disk_percent).toBe(50)
    expect(first.disk_reserved_bytes).toBe(5 * 1024 ** 3)
    expect(first.rules.map((rule) => rule.action)).toEqual(['bypass', 'cache'])
    expect(first.rules[0].id).not.toBe(second.rules[0].id)
    expect(
      cacheConfigSchema.safeParse({ ...first, memory_percent: 101 }).success
    ).toBe(false)
    expect(
      cacheConfigSchema.safeParse({ ...first, disk_reserved_bytes: -1 }).success
    ).toBe(false)
  })
  it('allows only meaningful operation scopes and full URLs', () => {
    const base = {
      operation: 'prewarm',
      scope: 'urls',
      object_id: '00000000-0000-4000-8000-000000000001',
      urls: ['https://example.com/a'],
    }
    expect(cacheOperationInputSchema.safeParse(base).success).toBe(true)
    for (const urls of [
      [],
      ['/a'],
      ['https://user:password@example.com/a'],
      ['https://example.com/a#fragment'],
    ])
      expect(
        cacheOperationInputSchema.safeParse({ ...base, urls }).success
      ).toBe(false)
    expect(
      cacheOperationInputSchema.safeParse({
        ...base,
        operation: 'clean',
        scope: 'all',
        urls: [],
      }).success
    ).toBe(true)
    expect(
      cacheOperationInputSchema.safeParse({
        ...base,
        operation: 'refresh',
        scope: 'prefix',
        urls: ['https://example.com/a?q=1'],
      }).success
    ).toBe(false)
  })
  it('requires a website policy when caching is enabled', () => {
    const result = websiteFormSchema.safeParse({
      ...defaultWebsiteConfig(),
      name: 'site',
      status: 'enabled',
      cluster_id: '00000000-0000-4000-8000-000000000001',
      domains: [
        {
          id: '00000000-0000-4000-8000-000000000002',
          hostname: 'example.com',
          dns_mode: 'external',
        },
      ],
      origins: [
        {
          id: '00000000-0000-4000-8000-000000000003',
          group: 'default',
          protocol: 'http',
          host: 'example.com',
          port: 80,
          role: 'primary',
          weight: 1,
          status: 'enabled',
        },
      ],
      cache_enabled: true,
    })
    expect(result.success).toBe(false)
    if (!result.success)
      expect(
        result.error.issues.some((issue) => issue.path[0] === 'cache_policy_id')
      ).toBe(true)
  })
})

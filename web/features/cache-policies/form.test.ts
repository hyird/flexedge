import { describe, expect, it } from 'vitest'
import {
  BYTE_UNITS,
  DURATION_UNITS,
  MAX_SAFE_INTEGER,
  cachePolicySchema,
  cacheRuleSchema,
  formatUnitValue,
  unitValueToBase,
} from './form'
import {
  cloneCacheRule,
  createCacheRule,
  createDefaultCacheRule,
} from './utils'

const rule = {
  id: crypto.randomUUID(),
  name: 'images',
  status: 'enabled' as const,
  match_type: 'prefix' as const,
  patterns: ['/assets'],
  action: 'cache' as const,
  ttl_seconds: 3600,
  query_mode: 'include' as const,
  status_codes: [200, 301],
  min_object_bytes: 0,
  max_object_bytes: 268435456,
  stale_if_error_seconds: 60,
  ignore_origin_cache_control: false,
  range_enabled: true,
}
const defaultRule = createDefaultCacheRule()
describe('cache policy editor contract', () => {
  it('round trips every rule field', () => {
    const result = cachePolicySchema.parse({
      name: 'web',
      description: 'x',
      status: 'enabled',
      rules: [rule, defaultRule],
    })
    expect(result.rules[0]).toEqual(rule)
  })
  it('uses the positive ignore field without the legacy alias', () => {
    const parsedRule = cacheRuleSchema.parse({
      ...rule,
      ignore_origin_cache_control: true,
    })
    expect(parsedRule.ignore_origin_cache_control).toBe(true)
    expect('respect_origin_cache_control' in parsedRule).toBe(false)

    const parsedPolicy = cachePolicySchema.parse({
      name: 'web',
      description: '',
      status: 'enabled',
      rules: [parsedRule, defaultRule],
    })
    expect(parsedPolicy.rules[0].ignore_origin_cache_control).toBe(true)
    expect('respect_origin_cache_control' in parsedPolicy.rules[0]).toBe(false)
  })
  it('rejects patterns for non all rules and invalid TTL bounds', () => {
    expect(
      cachePolicySchema.safeParse({
        name: 'x',
        description: '',
        status: 'enabled',
        rules: [{ ...rule, patterns: [] }, defaultRule],
      }).success
    ).toBe(false)
    expect(
      cachePolicySchema.safeParse({
        name: 'x',
        description: '',
        status: 'enabled',
        rules: [{ ...rule, ttl_seconds: 31536001 }, defaultRule],
      }).success
    ).toBe(false)
  })

  it('requires the immutable default rule at the end of the list', () => {
    expect(
      cachePolicySchema.safeParse({
        name: 'x',
        description: '',
        status: 'enabled',
        rules: [rule],
      }).success
    ).toBe(false)
    expect(
      cachePolicySchema.safeParse({
        name: 'x',
        description: '',
        status: 'enabled',
        rules: [defaultRule, rule],
      }).success
    ).toBe(false)
    expect(
      cachePolicySchema.safeParse({
        name: 'x',
        description: '',
        status: 'enabled',
        rules: [rule, { ...defaultRule, action: 'bypass' }],
      }).success
    ).toBe(false)
  })

  it('creates the editable built-in fallback rule', () => {
    expect(defaultRule).toMatchObject({
      id: '00000000-0000-4000-8000-000000000000',
      name: '默认规则',
      status: 'enabled',
      match_type: 'all',
      patterns: [],
      action: 'cache',
      ttl_seconds: 0,
      query_mode: 'include',
      status_codes: [200, 301, 302, 404],
      ignore_origin_cache_control: false,
      range_enabled: true,
    })
  })

  it('round trips non-divisible values through a display unit', () => {
    const minute = DURATION_UNITS.find((unit) => unit.value === 'minutes')!
    const kibibyte = BYTE_UNITS.find((unit) => unit.value === 'kibibytes')!
    const seconds = 61
    const bytes = 12345

    expect(
      unitValueToBase(formatUnitValue(seconds, minute), minute, 31536000)
    ).toBe(seconds)
    expect(
      unitValueToBase(
        formatUnitValue(bytes, kibibyte),
        kibibyte,
        MAX_SAFE_INTEGER
      )
    ).toBe(bytes)
    expect(
      unitValueToBase(
        '1.5',
        DURATION_UNITS.find((unit) => unit.value === 'hours')!,
        31536000
      )
    ).toBe(5400)
    expect(unitValueToBase('0.1', kibibyte, MAX_SAFE_INTEGER)).toBe(undefined)
    expect(
      unitValueToBase('1000000000000000.5', BYTE_UNITS[0], MAX_SAFE_INTEGER)
    ).toBe(undefined)
    expect(
      unitValueToBase('9007199254740992', BYTE_UNITS[0], MAX_SAFE_INTEGER)
    ).toBe(undefined)
  })

  it('keeps a draft rule id while cloning editable arrays', () => {
    const draft = cloneCacheRule(rule)
    draft.patterns.push('/new')
    draft.status_codes.push(404)

    expect(draft.id).toBe(rule.id)
    expect(rule.patterns).toEqual(['/assets'])
    expect(rule.status_codes).toEqual([200, 301])
  })

  it('defaults new cache rules to independent static file extension rules', () => {
    const first = createCacheRule()
    const second = createCacheRule()
    const bypass = createCacheRule('bypass')

    expect(first.name).toBe('静态文件')
    expect(first.match_type).toBe('extension')
    expect(first.patterns).toContain('.css')
    expect(first.patterns).toContain('.js')
    expect(first.patterns).toContain('.woff2')
    expect(first.patterns).toContain('.pdf')
    expect(first.patterns).toContain('.zip')
    expect(first.patterns).not.toContain('.json')
    expect(first.patterns).not.toContain('.html')
    expect(first.ttl_seconds).toBe(3600)
    expect(first.id).not.toBe(second.id)
    expect(first.patterns).not.toBe(second.patterns)
    expect(first.status_codes).not.toBe(second.status_codes)

    first.patterns.push('.custom')
    first.status_codes.push(301)
    expect(second.patterns).not.toContain('.custom')
    expect(second.status_codes).toEqual([200])

    expect(bypass.name).toBe('绕过条件')
    expect(bypass.match_type).toBe('all')
    expect(bypass.patterns).toEqual([])
    expect(bypass.ttl_seconds).toBe(0)
  })

  it('rejects backend-invalid pattern, status and object range drafts', () => {
    expect(
      cacheRuleSchema.safeParse({ ...rule, patterns: ['/images?format=webp'] })
        .success
    ).toBe(false)
    expect(
      cacheRuleSchema.safeParse({ ...rule, status_codes: [200, 500] }).success
    ).toBe(false)
    expect(
      cacheRuleSchema.safeParse({
        ...rule,
        min_object_bytes: 20,
        max_object_bytes: 10,
      }).success
    ).toBe(false)
  })

  it('uses UTF-8 byte limits for policy and rule labels', () => {
    expect(
      cacheRuleSchema.safeParse({ ...rule, name: '字'.repeat(51) }).success
    ).toBe(false)
    expect(
      cachePolicySchema.safeParse({
        name: '名'.repeat(67),
        description: '',
        status: 'enabled',
        rules: [defaultRule],
      }).success
    ).toBe(false)
    expect(
      cachePolicySchema.safeParse({
        name: 'x',
        description: '字'.repeat(667),
        status: 'enabled',
        rules: [defaultRule],
      }).success
    ).toBe(false)
  })
})

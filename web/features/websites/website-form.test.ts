import { describe, expect, it } from 'vitest'
import {
  defaultWebsiteConfig,
  routeRuleToForm,
  websiteFormSchema,
} from './website-form'

function values() {
  return {
    ...defaultWebsiteConfig(),
    name: 'test',
    status: 'enabled',
    cluster_id: '12345678-1234-4234-8234-123456789012',
    domains: [
      {
        id: '12345678-1234-4234-8234-123456789013',
        hostname: 'example.com',
        dns_mode: 'external',
      },
    ],
    origins: [
      {
        id: '12345678-1234-4234-8234-123456789014',
        group: 'default',
        protocol: 'http',
        host: 'example.com',
        port: 80,
        role: 'primary',
        weight: 1,
        status: 'enabled',
      },
    ],
  }
}

describe('website settings validation', () => {
  it('round trips rule metadata and normalizes domain filters', () => {
    const rule = routeRuleToForm({
      id: '12345678-1234-4234-8234-123456789015',
      origin_group: 'default',
      path: '/api',
      name: 'API',
      description: 'notes',
      hostnames: ['API.Example.com', ''],
      rewrite_mode: 'replace_prefix',
      rewrite_path: '/v2',
      query_mode: 'replace',
      query_string: 'v=2',
    })
    const result = websiteFormSchema.parse({ ...values(), route_rules: [rule] })
      .route_rules[0]
    expect(result.hostnames).toEqual(['api.example.com'])
    expect(result.name).toBe('API')
    expect(result.description).toBe('notes')
    expect(result.rewrite_mode).toBe('replace_prefix')
    expect(result.query_string).toBe('v=2')
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        route_rules: [{ ...rule, hostnames: ['*.example.com'] }],
      }).success
    ).toBe(false)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        route_rules: [{ ...rule, match_type: 'exact' }],
      }).success
    ).toBe(false)
  })
  it('maps existing rules to the original path and query behavior', () => {
    expect(routeRuleToForm({ rewrite_path: '/old' }).rewrite_mode).toBe(
      'replace_path'
    )
    expect(routeRuleToForm({}).rewrite_mode).toBe('none')
    expect(routeRuleToForm({}).query_mode).toBe('preserve')
  })
  it('keeps editable origin timeout values', () => {
    const result = websiteFormSchema.parse({
      ...values(),
      origin_connect_timeout_seconds: 25,
      origin_read_timeout_seconds: 120,
    })
    expect(result.origin_connect_timeout_seconds).toBe(25)
    expect(result.origin_read_timeout_seconds).toBe(120)
  })
  it('rejects invalid timeouts and compression options', () => {
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        origin_connect_timeout_seconds: 301,
      }).success
    ).toBe(false)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        origin_read_timeout_seconds: 601,
      }).success
    ).toBe(false)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        response_compression_algorithms: [],
      }).success
    ).toBe(false)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        response_compression_max_bytes: 512,
      }).success
    ).toBe(false)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        response_compression_algorithms: ['gzip', 'gzip'],
      }).success
    ).toBe(false)
  })
})

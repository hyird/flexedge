import { describe, expect, it } from 'vitest'
import {
  defaultWebsiteConfig,
  routeRuleToForm,
  setWebsiteHttpsEnabled,
  websiteCertificateOptions,
  websiteFormSchema,
} from './website-form'
import { websiteFormToConfig } from './website-form-mapping'

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
  it('requires certificate bindings exactly when HTTPS is enabled', () => {
    const certificate_ids = ['12345678-1234-4234-8234-123456789020']
    for (const input of [
      { ...values(), https_enabled: true },
      { ...values(), https_enabled: false, certificate_ids },
    ]) {
      const result = websiteFormSchema.safeParse(input)
      expect(result.success).toBe(false)
      if (!result.success)
        expect(
          result.error.issues.map((issue) => issue.path.join('.'))
        ).toContain('certificate_ids')
    }
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        https_enabled: true,
        certificate_ids,
      }).success
    ).toBe(true)
  })
  it('produces a valid HTTP-only save payload after disabling configured TLS', () => {
    const configured = websiteFormSchema.parse({
      ...values(),
      https_enabled: true,
      force_https: true,
      hsts_enabled: true,
      certificate_ids: ['12345678-1234-4234-8234-123456789020'],
    })
    const disabled = setWebsiteHttpsEnabled(configured, false)
    const payload = websiteFormToConfig(websiteFormSchema.parse(disabled))
    expect(payload).toMatchObject({
      https_enabled: false,
      certificate_ids: [],
      force_https: false,
      hsts_enabled: false,
    })
    expect(payload.domains).toEqual(configured.domains)
    expect(payload.origins).toEqual(configured.origins)
    expect(configured.certificate_ids).toEqual([
      '12345678-1234-4234-8234-123456789020',
    ])
    expect(
      websiteFormSchema.safeParse(setWebsiteHttpsEnabled(disabled, true))
        .success
    ).toBe(false)
  })
  it('keeps unavailable bindings removable and drops them after explicit removal', () => {
    const available = [{ id: 'new', domains: ['example.com'] }]
    const attached = [
      { id: 'expired', domains: ['old.example.com'], usable: false },
      { id: 'new', domains: ['example.com'], usable: true },
    ]
    expect(
      websiteCertificateOptions(
        available,
        ['expired', 'new', 'deleted'],
        attached
      )
    ).toEqual([
      { id: 'new', domains: ['example.com'], usable: true },
      { id: 'expired', domains: ['old.example.com'], usable: false },
      { id: 'deleted', domains: [], usable: false },
    ])
    expect(websiteCertificateOptions(available, ['new'], attached)).toEqual([
      { id: 'new', domains: ['example.com'], usable: true },
    ])
  })
  it('accepts standalone rewrites and rejects origin/header options on them', () => {
    const rule = routeRuleToForm({
      action: 'rewrite',
      match_type: 'prefix',
      path: '/old',
      rewrite_mode: 'replace_prefix',
      rewrite_path: '/api',
    })
    expect(
      websiteFormSchema.safeParse({ ...values(), route_rules: [rule] }).success
    ).toBe(true)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        route_rules: [{ ...rule, origin_group: 'default' }],
      }).success
    ).toBe(false)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        route_rules: [{ ...rule, request_headers_text: 'X-Test: value' }],
      }).success
    ).toBe(false)
  })
  it('round trips suffix, regex conditions and method-preserving redirects', () => {
    const rule = routeRuleToForm({
      id: '12345678-1234-4234-8234-123456789015',
      match_type: 'regex',
      path: '^/old/(.*)$',
      action: 'redirect',
      redirect_status: 308,
      redirect_url: '/new/${1}',
      conditions: [
        { source: 'query', name: 'tag', op: 'equals', value: 'two' },
      ],
    })
    const parsed = websiteFormSchema.parse({ ...values(), route_rules: [rule] })
      .route_rules[0]
    expect(parsed.match_type).toBe('regex')
    expect(parsed.redirect_status).toBe(308)
    expect(parsed.conditions).toEqual([
      { source: 'query', name: 'tag', op: 'equals', value: 'two' },
    ])
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        route_rules: [{ ...rule, redirect_status: 307 }],
      }).success
    ).toBe(true)
    expect(
      websiteFormSchema.safeParse({
        ...values(),
        route_rules: [
          {
            ...rule,
            match_type: 'suffix',
            path: '.jpg',
            redirect_url: '/image',
          },
        ],
      }).success
    ).toBe(true)
    for (const patch of [
      { path: '(?<=a)b' },
      { redirect_url: '/${2}' },
      { redirect_status: 303 },
      {
        conditions: [
          { source: 'header', name: 'X Bad', op: 'equals', value: 'x' },
        ],
      },
    ])
      expect(
        websiteFormSchema.safeParse({
          ...values(),
          route_rules: [{ ...rule, ...patch }],
        }).success
      ).toBe(false)
  })
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

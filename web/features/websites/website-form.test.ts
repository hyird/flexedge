import { describe, expect, it } from 'vitest'
import { uncoveredHostnames } from '@/features/certificates/certificate-coverage'
import {
  defaultWebsiteConfig,
  routeRuleToForm,
  setWebsiteHttpsEnabled,
  validateWebsiteForm,
  websiteCertificateOptions,
  websiteFormSchema,
  websiteHostnames,
} from './website-form'
import {
  websiteFormToConfig,
  websiteToFormValues,
} from './website-form-mapping'
import { collectWebsiteFormErrors } from './website-sections'

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
  it('uses the selected cache policy as the only enablement choice', () => {
    const policyId = '12345678-1234-4234-8234-123456789099'
    const selected = websiteFormSchema.parse({
      ...values(),
      cache_policy_id: policyId,
      cache_enabled: false,
    })
    expect(websiteFormToConfig(selected).cache_enabled).toBe(true)
    expect(
      websiteFormToConfig({
        ...selected,
        cache_policy_id: '',
        cache_enabled: false,
      }).cache_enabled
    ).toBe(false)
    const disabled = websiteToFormValues({
      cluster_id: selected.cluster_id,
      status: 'enabled',
      config: {
        ...defaultWebsiteConfig(),
        cache_policy_id: policyId,
        cache_enabled: false,
      },
    })
    expect(disabled.cache_policy_id).toBe('')
    expect(disabled.cache_enabled).toBe(false)
  })
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
    const hostnames = ['example.com', 'old.example.com']
    expect(
      websiteCertificateOptions(
        available,
        ['expired', 'new', 'deleted'],
        attached,
        hostnames
      )
    ).toEqual([
      { id: 'new', domains: ['example.com'], usable: true, matched: true },
      {
        id: 'expired',
        domains: ['old.example.com'],
        usable: false,
        matched: true,
      },
      { id: 'deleted', domains: [], usable: false, matched: false },
    ])
    expect(
      websiteCertificateOptions(available, ['new'], attached, hostnames)
    ).toEqual([
      { id: 'new', domains: ['example.com'], usable: true, matched: true },
    ])
  })

  describe('website certificate picker', () => {
    const wildcard = {
      id: '12345678-1234-4234-8234-123456789031',
      domains: ['*.example.com'],
    }
    const apex = {
      id: '12345678-1234-4234-8234-123456789032',
      domains: ['example.com', 'api.example.com'],
    }
    const other = {
      id: '12345678-1234-4234-8234-123456789033',
      domains: ['*.other.test'],
    }
    const available = [wildcard, apex, other]
    function withDomains(hostnames: string[], certificateIds: string[]) {
      const base = values()
      return {
        ...base,
        domains: hostnames.map((hostname, index) => ({
          ...base.domains[0],
          id: `12345678-1234-4234-8234-12345678904${index}`,
          hostname,
        })),
        https_enabled: true,
        certificate_ids: certificateIds,
      }
    }
    const ids = (options: { id: string }[]) => options.map(({ id }) => id)

    it('lists usable certificates matching at least one bound domain', () => {
      expect(
        ids(websiteCertificateOptions(available, [], [], ['www.example.com']))
      ).toEqual([wildcard.id])
      expect(
        ids(websiteCertificateOptions(available, [], [], ['example.com']))
      ).toEqual([apex.id])
      // Adding a domain re-filters from the live draft.
      const draft = withDomains(['www.example.com', 'API.example.com.'], [])
      expect(websiteHostnames(draft)).toEqual([
        'www.example.com',
        'API.example.com.',
      ])
      expect(
        ids(
          websiteCertificateOptions(available, [], [], websiteHostnames(draft))
        )
      ).toEqual([wildcard.id, apex.id])
    })

    it('lists nothing without bound domains', () => {
      expect(websiteHostnames(withDomains(['', '  '], []))).toEqual([])
      expect(websiteCertificateOptions(available, [], [], [])).toEqual([])
    })

    it('keeps a selected certificate that no longer matches and blocks saving', () => {
      const options = websiteCertificateOptions(
        available,
        [other.id],
        [],
        ['www.example.com']
      )
      expect(options).toEqual([
        { ...wildcard, usable: true, matched: true },
        { ...other, usable: true, matched: false },
      ])
      const draft = withDomains(['www.example.com'], [other.id])
      expect(websiteFormSchema.safeParse(draft).success).toBe(true)
      // The options snapshot and the attached bindings may repeat a certificate.
      const result = validateWebsiteForm(draft, undefined, [
        ...available,
        other,
      ])
      expect(result.success).toBe(false)
      expect(result.error?.issues).toEqual([
        expect.objectContaining({
          path: ['certificate_ids'],
          message: '证书未匹配网站域名：*.other.test',
        }),
      ])
    })

    it('allows saving while some domains stay uncovered', () => {
      const draft = withDomains(
        ['www.example.com', 'example.com'],
        [wildcard.id]
      )
      expect(validateWebsiteForm(draft, undefined, available).success).toBe(
        true
      )
      expect(uncoveredHostnames([wildcard], websiteHostnames(draft))).toEqual([
        'example.com',
      ])
      expect(
        uncoveredHostnames([wildcard, apex], websiteHostnames(draft))
      ).toEqual([])
    })
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
  it('maps stored empty modes to the original path and query behavior', () => {
    expect(
      routeRuleToForm({ rewrite_mode: '', rewrite_path: '/old' }).rewrite_mode
    ).toBe('replace_path')
    expect(routeRuleToForm({ rewrite_mode: '' }).rewrite_mode).toBe('none')
    expect(routeRuleToForm({ query_mode: '' }).query_mode).toBe('preserve')
  })
  it('limits each route rule to 20 request and 20 response headers', () => {
    const headers = (count: number) =>
      Array.from({ length: count }, (_, index) => `X-H${index}: v`).join('\n')
    const rule = routeRuleToForm({
      id: '12345678-1234-4234-8234-123456789015',
      origin_group: 'default',
    })
    expect(
      validateWebsiteForm({
        ...values(),
        route_rules: [
          {
            ...rule,
            request_headers_text: `${headers(20)}\n\n`,
            response_headers_text: headers(20),
          },
        ],
      }).success
    ).toBe(true)
    const result = validateWebsiteForm({
      ...values(),
      route_rules: [
        {
          ...rule,
          request_headers_text: headers(21),
          response_headers_text: headers(21),
        },
      ],
    })
    expect(result.success).toBe(false)
    if (!result.success)
      expect(collectWebsiteFormErrors(result.error.issues)).toMatchObject({
        'route_rules.0.request_headers_text': '最多20个请求头',
        'route_rules.0.response_headers_text': '最多20个响应头',
      })
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

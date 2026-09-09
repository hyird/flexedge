import { describe, expect, it } from 'vitest'
import { defaultWebsiteConfig, routeRuleToForm } from './website-form'
import {
  websiteFormToConfig,
  websiteToFormValues,
} from './website-form-mapping'

describe('website editor boundary', () => {
  it('creates independent starter entries for a new website', () => {
    const first = websiteToFormValues()
    const second = websiteToFormValues()
    expect(first.domains).toHaveLength(1)
    expect(first.origins).toHaveLength(1)
    expect(first.domains[0].id).not.toBe(second.domains[0].id)
    expect(first.origins[0].id).not.toBe(second.origins[0].id)
    expect(first.cluster_id).toBe('')
    expect(first.status).toBe('enabled')
  })

  it('round trips editable settings without sharing cached resource arrays', () => {
    const config = defaultWebsiteConfig()
    config.name = 'Existing site'
    config.domains = [
      {
        id: crypto.randomUUID(),
        hostname: 'example.test',
        dns_mode: 'external',
      },
    ]
    config.origins = [
      {
        id: crypto.randomUUID(),
        group: 'default',
        protocol: 'https',
        host: 'origin.test',
        port: 443,
        role: 'primary',
        weight: 100,
        status: 'enabled',
      },
    ]
    config.certificate_ids = ['cert-one']
    const values = websiteToFormValues({
      cluster_id: 'cluster-one',
      status: 'disabled',
      config,
    })
    const output = websiteFormToConfig(values)
    expect(output).toEqual(config)
    expect(output).not.toHaveProperty('cluster_id')
    expect(output).not.toHaveProperty('status')
    values.domains[0].hostname = 'edited.test'
    values.certificate_ids.push('cert-two')
    expect(config.domains[0].hostname).toBe('example.test')
    expect(output.domains[0].hostname).toBe('example.test')
    expect(config.certificate_ids).toEqual(['cert-one'])
    expect(output.certificate_ids).toEqual(['cert-one'])
  })

  it('converts editor-only header text into API header entries', () => {
    const values = websiteToFormValues()
    values.route_rules = [
      routeRuleToForm({
        id: crypto.randomUUID(),
        action: 'proxy',
        request_headers: [{ name: 'X-Request', value: 'one' }],
        response_headers: [{ name: 'X-Response', value: 'two' }],
      }),
    ]
    const rule = websiteFormToConfig(values).route_rules[0]
    expect(rule.request_headers).toEqual([{ name: 'X-Request', value: 'one' }])
    expect(rule.response_headers).toEqual([
      { name: 'X-Response', value: 'two' },
    ])
    expect(rule).not.toHaveProperty('request_headers_text')
    expect(rule).not.toHaveProperty('response_headers_text')
  })
})

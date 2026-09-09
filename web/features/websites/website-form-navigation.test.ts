import type { FieldErrors } from 'react-hook-form'
import { describe, expect, it } from 'vitest'
import { routeRuleToForm, type WebsiteFormValues } from './website-form'
import { websiteErrorTarget } from './website-form-navigation'

const error = { type: 'required', message: 'required' }
const rules = ['redirect', 'rewrite', 'proxy'].map((action) =>
  routeRuleToForm({ action })
)

describe('website validation navigation', () => {
  it('routes representative fields to the section that renders them', () => {
    const cases = {
      cluster_id: 'basic',
      domains: 'domains',
      origins: 'origins',
      pass_client_ip: 'origin-settings',
      origin_read_timeout_seconds: 'origin-settings',
      health_check_path: 'health',
      unhealthy_threshold: 'health',
      certificate_ids: 'https',
      access_log_cookies: 'logs',
      response_compression_algorithms: 'compression',
    } as const
    for (const [field, section] of Object.entries(cases)) {
      expect(websiteErrorTarget({ [field]: error }, rules)).toEqual({
        section,
        routeIndex: null,
      })
    }
  })

  it('opens the failing row in its actual route phase', () => {
    for (let index = 0; index < rules.length; index += 1) {
      const errors: FieldErrors<WebsiteFormValues> = {
        route_rules: { [index]: { path: error } },
      }
      expect(websiteErrorTarget(errors, rules)).toEqual({
        section: rules[index].action,
        routeIndex: index,
      })
    }
  })

  it('does not retain a route expansion when another field becomes the first error', () => {
    expect(
      websiteErrorTarget(
        { name: error, route_rules: { 1: { path: error } } },
        rules
      )
    ).toEqual({ section: 'basic', routeIndex: null })
  })

  it('does not expand a missing row for array-level errors or removed rows', () => {
    expect(websiteErrorTarget({ route_rules: { root: error } }, rules)).toEqual(
      { section: 'redirect', routeIndex: null }
    )
    expect(
      websiteErrorTarget({ route_rules: { 5: { path: error } } }, rules)
    ).toEqual({ section: 'redirect', routeIndex: null })
    expect(websiteErrorTarget({ route_rules: { root: error } }, [])).toEqual({
      section: 'proxy',
      routeIndex: null,
    })
  })

  it('handles form-level errors and an empty error collection', () => {
    expect(websiteErrorTarget({ root: error }, rules)).toEqual({
      section: 'basic',
      routeIndex: null,
    })
    expect(websiteErrorTarget({}, rules)).toEqual({
      section: 'basic',
      routeIndex: null,
    })
  })
})

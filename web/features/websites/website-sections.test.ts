import { describe, expect, it } from 'vitest'
import {
  defaultWebsiteConfig,
  validateWebsiteForm,
  websiteFormSchema,
} from './website-form'
import {
  collectWebsiteFormErrors,
  countWebsiteSectionErrors,
  firstWebsiteErrorTarget,
  routeRuleErrors,
  websiteSectionForPath,
  websiteSectionIds,
} from './website-sections'

describe('website settings sections', () => {
  it('lists the nine groups in navigation order', () => {
    expect(websiteSectionIds).toEqual([
      'basic',
      'domains',
      'https',
      'origins',
      'origin-settings',
      'health',
      'routes',
      'compression',
      'logs',
    ])
  })

  it('maps nested error paths to their owning group', () => {
    expect(websiteSectionForPath('origins.2.host')).toBe('origins')
    expect(websiteSectionForPath('default_origin_group')).toBe('origins')
    expect(websiteSectionForPath('origin_host_header')).toBe('origin-settings')
    expect(websiteSectionForPath('route_rules.0.path')).toBe('routes')
    expect(websiteSectionForPath('certificate_ids')).toBe('https')
    expect(websiteSectionForPath('access_log_status_code_ranges')).toBe('logs')
    expect(websiteSectionForPath('unknown')).toBe('basic')
  })

  it('targets the earliest failing group and route rule', () => {
    const errors = {
      'route_rules.3.path': '请输入匹配路径',
      'route_rules.1.redirect_url': '跳转地址需以 /、http:// 或 https:// 开头',
      response_compression_algorithms: '启用压缩时至少选择一种算法',
    }
    expect(firstWebsiteErrorTarget(errors)).toEqual({
      section: 'routes',
      routeRuleIndex: 1,
    })
    expect(
      firstWebsiteErrorTarget({ ...errors, 'domains.0.hostname': '请输入域名' })
    ).toEqual({ section: 'domains' })
    expect(firstWebsiteErrorTarget({})).toBeUndefined()
    expect(countWebsiteSectionErrors(errors)).toEqual({
      routes: 2,
      compression: 1,
    })
    expect(routeRuleErrors(errors, 3)).toEqual({ path: '请输入匹配路径' })
  })

  it('collects the first schema message per field from a failed parse', () => {
    const result = websiteFormSchema.safeParse({
      ...defaultWebsiteConfig(),
      cluster_id: '',
      status: 'enabled',
      domains: [],
    })
    expect(result.success).toBe(false)
    if (result.success) return
    const errors = collectWebsiteFormErrors(result.error.issues)
    expect(errors.cluster_id).toBe('请选择所属集群')
    expect(errors.domains).toBe('至少添加一个域名')
    expect(firstWebsiteErrorTarget(errors)).toEqual({ section: 'basic' })
  })

  it('reports built-in range errors in Chinese while keeping custom messages', () => {
    const result = validateWebsiteForm({
      ...defaultWebsiteConfig(),
      cluster_id: '',
      status: 'enabled',
      domains: [],
      origin_connect_timeout_seconds: 0,
    })
    expect(result.success).toBe(false)
    if (result.success) return
    const errors = collectWebsiteFormErrors(result.error.issues)
    expect(errors.cluster_id).toBe('请选择所属集群')
    expect(errors.origin_connect_timeout_seconds).toMatch(/^数值过小/)
  })
})

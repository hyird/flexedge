import { describe, expect, it } from 'vitest'
import { buildBreadcrumbTrail } from './breadcrumb-trail'

describe('breadcrumb trail', () => {
  it('starts from the navigation section and skips the duplicate list crumb', () => {
    expect(
      buildBreadcrumbTrail(
        '/websites/',
        [
          { id: '/_authenticated', pathname: '/' },
          {
            id: '/_authenticated/websites/',
            pathname: '/websites/',
            breadcrumb: '网站',
          },
        ],
        new Map()
      )
    ).toEqual([{ key: '/websites', label: '网站' }])
  })

  it('uses runtime resource labels and links every crumb but the current page', () => {
    const matches = [
      { id: '/_authenticated', pathname: '/' },
      {
        id: '/_authenticated/websites/$websiteId',
        pathname: '/websites/w1',
        breadcrumb: '网站详情',
      },
      { id: '/_authenticated/websites/$websiteId/', pathname: '/websites/w1/' },
      {
        id: '/_authenticated/websites/$websiteId/settings',
        pathname: '/websites/w1/settings',
        breadcrumb: '配置',
      },
    ]
    expect(
      buildBreadcrumbTrail(
        '/websites/w1/settings',
        matches,
        new Map([['/_authenticated/websites/$websiteId', 'QA 网站']])
      )
    ).toEqual([
      { key: '/websites', label: '网站', href: '/websites' },
      {
        key: '/_authenticated/websites/$websiteId',
        label: 'QA 网站',
        href: '/websites/w1',
      },
      { key: '/_authenticated/websites/$websiteId/settings', label: '配置' },
    ])
  })

  it('keeps the overview root for the index route', () => {
    expect(
      buildBreadcrumbTrail(
        '/',
        [{ id: '/_authenticated/', pathname: '/' }],
        new Map()
      )
    ).toEqual([{ key: '/', label: '概览' }])
  })
})

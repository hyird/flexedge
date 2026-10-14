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

  it('shows the section and the resource name, whatever in-page tab is open', () => {
    const matches = [
      { id: '/_authenticated', pathname: '/' },
      {
        id: '/_authenticated/websites/$websiteId',
        pathname: '/websites/w1',
        breadcrumb: '网站详情',
      },
    ]
    expect(
      buildBreadcrumbTrail(
        '/websites/w1',
        matches,
        new Map([['/_authenticated/websites/$websiteId', 'QA 网站']])
      )
    ).toEqual([
      { key: '/websites', label: '网站', href: '/websites' },
      { key: '/_authenticated/websites/$websiteId', label: 'QA 网站' },
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

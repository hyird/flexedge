import { describe, expect, it } from 'vitest'
import {
  certificateCoversHostname,
  certificateMatchesHostnames,
  uncoveredHostnames,
} from './certificate-coverage'

// Same cases as tests/website_certificate_coverage.cpp.
describe('certificate hostname coverage', () => {
  it('matches exact names ignoring case and trailing dots', () => {
    expect(certificateCoversHostname('a-z.xin', 'a-z.xin')).toBe(true)
    expect(certificateCoversHostname('A-Z.XIN.', 'a-z.xin')).toBe(true)
    expect(certificateCoversHostname('a-z.xin', 'A-Z.Xin.')).toBe(true)
    expect(certificateCoversHostname('a-z.xin', 't.a-z.xin')).toBe(false)
    expect(certificateCoversHostname('', 'a-z.xin')).toBe(false)
  })

  it('lets a wildcard cover exactly one extra left-most label', () => {
    expect(certificateCoversHostname('*.a-z.xin', 't.a-z.xin')).toBe(true)
    expect(certificateCoversHostname('*.A-Z.xin', 'T.a-z.xin.')).toBe(true)
    expect(certificateCoversHostname('*.a-z.xin', 'a-z.xin')).toBe(false)
    expect(certificateCoversHostname('*.a-z.xin', 'x.t.a-z.xin')).toBe(false)
    expect(certificateCoversHostname('*.a-z.xin', 'ta-z.xin')).toBe(false)
  })

  it('covers a wildcard website domain only with the same wildcard', () => {
    expect(certificateCoversHostname('*.a-z.xin', '*.a-z.xin')).toBe(true)
    expect(certificateCoversHostname('*.a-z.xin', '*.t.a-z.xin')).toBe(false)
    expect(certificateCoversHostname('t.a-z.xin', '*.a-z.xin')).toBe(false)
    expect(certificateCoversHostname('a-z.xin', '*.a-z.xin')).toBe(false)
  })

  it('matches multi-name certificates against any bound hostname', () => {
    const names = ['other.example', 'T.A-Z.XIN.']
    expect(certificateMatchesHostnames(names, ['a-z.xin', 't.a-z.xin'])).toBe(
      true
    )
    expect(certificateMatchesHostnames(names, ['x.a-z.xin'])).toBe(false)
    expect(certificateMatchesHostnames(names, [])).toBe(false)
  })

  it('requires every hostname to be covered by some certificate', () => {
    const hostnames = ['a-z.xin', 't.a-z.xin', '*.a-z.xin']
    expect(uncoveredHostnames([{ domains: ['*.a-z.xin'] }], hostnames)).toEqual(
      ['a-z.xin']
    )
    expect(
      uncoveredHostnames(
        [{ domains: ['*.a-z.xin'] }, { domains: ['a-z.xin'] }],
        hostnames
      )
    ).toEqual([])
    expect(uncoveredHostnames([], hostnames)).toEqual(hostnames)
  })
})

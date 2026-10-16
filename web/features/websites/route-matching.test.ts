import { describe, expect, it } from 'vitest'
import { compileRoutePattern, validCaptureTemplate } from './route-matching'

describe('route pattern validation', () => {
  it('validates RE2 syntax, capture limits and replacement references', () => {
    const pattern = compileRoutePattern('^/old/([^/]+)/(.*)$')
    expect(pattern.groupCount()).toBe(2)
    expect(
      validCaptureTemplate('/new/${2}/${1}?cash=$$', pattern.groupCount())
    ).toBe(true)
    expect(validCaptureTemplate('/${3}', 2)).toBe(false)
    expect(validCaptureTemplate('/${10}', 2)).toBe(false)
    expect(validCaptureTemplate('/${broken}', 2)).toBe(false)
    expect(validCaptureTemplate('/$${1}', 0)).toBe(true)
    for (const regex of [
      '(?<=a)b',
      '(a)\\1',
      '[',
      'a'.repeat(513),
      '(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)',
    ])
      expect(() => compileRoutePattern(regex)).toThrow()
    expect(
      compileRoutePattern('(a+)+$')
        .matcher('a'.repeat(100000) + '!')
        .find()
    ).toBe(false)
  })
})

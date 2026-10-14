import { expect, test } from 'vitest'
import { ApiBusinessError } from '@/lib/api-response'
import {
  formatFingerprint,
  isCertificateNotFound,
  isExpiring,
  validityProgress,
} from './certificate-display'
import type { Certificate } from './types'

const base = {
  id: 'certificate',
  revision: 1,
  created_at: '',
  updated_at: '',
  domains: ['example.com'],
  issuer: "Let's Encrypt",
  certificate_provider_id: 'provider',
  certificate_provider: 'letsencrypt',
  status: 'valid',
  usable: true,
  config: { auto_renew: true },
  dns_zone_id: 'zone',
  dns_zone_domain: 'example.com',
  website_count: 0,
} satisfies Certificate

test('the expiring highlight matches the overview expiring count', () => {
  expect(isExpiring({ ...base, remaining_days: 30 })).toBe(true)
  expect(isExpiring({ ...base, remaining_days: 31 })).toBe(false)
  expect(isExpiring({ ...base, status: 'renewing', remaining_days: 3 })).toBe(
    false
  )
  expect(isExpiring(base)).toBe(false)
})

test('validity progress reports the remaining share and urgency tone', () => {
  const window = {
    not_before: '2026-10-01T00:00:00Z',
    expires_at: '2026-12-30T00:00:00Z',
  }
  expect(validityProgress({ ...base, ...window, remaining_days: 45 })).toEqual({
    totalDays: 90,
    remainingDays: 45,
    percent: 50,
    tone: 'success',
  })
  expect(
    validityProgress({ ...base, ...window, remaining_days: 12 })?.tone
  ).toBe('warning')
  expect(
    validityProgress({ ...base, ...window, remaining_days: 7 })?.tone
  ).toBe('danger')
  expect(validityProgress(base)).toBeUndefined()
})

test('fingerprints display as colon-separated upper-case pairs', () => {
  expect(formatFingerprint('ab01cd')).toBe('AB:01:CD')
  expect(formatFingerprint('AB:01')).toBe('AB:01')
})

test('only the certificate not-found business code means the resource is gone', () => {
  expect(
    isCertificateNotFound(new ApiBusinessError(16001, '证书任务不存在'))
  ).toBe(true)
  expect(isCertificateNotFound(new ApiBusinessError(40001, 'x'))).toBe(false)
  expect(isCertificateNotFound(new Error('x'))).toBe(false)
})

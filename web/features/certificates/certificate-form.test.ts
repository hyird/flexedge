import { expect, test } from 'vitest'
import { certificateFormSchema } from './certificate-form'

const values = {
  domain: '*.example.com',
  certificate_provider_id: '29b9df01-e8bb-4f33-87e5-dfef34c77f8f',
  dns_zone_id: '29b9df01-e8bb-4f33-87e5-dfef34c77f8f',
  auto_renew: true,
}
test('certificate domain labels follow backend boundaries', () => {
  for (const domain of [
    '*.example.com',
    'a.com',
    'a-b.com',
    'a'.repeat(63) + '.com',
  ])
    expect(
      certificateFormSchema().safeParse({ ...values, domain }).success
    ).toBe(true)
  for (const domain of [
    '-a.com',
    'a-.com',
    'a'.repeat(64) + '.com',
    'a..com',
    'foo.*.com',
  ])
    expect(
      certificateFormSchema().safeParse({ ...values, domain }).success
    ).toBe(false)
})
test('renewal settings validate only the mutable field', () => {
  const settings = {
    domain: '',
    certificate_provider_id: '',
    dns_zone_id: '',
    auto_renew: false,
  }
  expect(certificateFormSchema(true).safeParse(settings).success).toBe(true)
  expect(certificateFormSchema().safeParse(settings).success).toBe(false)
  expect(
    certificateFormSchema(true).safeParse({ ...settings, auto_renew: 'false' })
      .success
  ).toBe(false)
})

import { expect, test } from 'vitest'
import { certificateProviderFormSchema } from './certificate-provider-form'

const keyValues = {
  provider: 'zerossl',
  credential_mode: 'access_key',
  account_email: '',
  access_key: '',
}
test('only editing an existing key account permits retaining a blank key', () => {
  expect(certificateProviderFormSchema().safeParse(keyValues).success).toBe(
    false
  )
  expect(
    certificateProviderFormSchema({ credential_mode: 'email' }).safeParse(
      keyValues
    ).success
  ).toBe(false)
  expect(
    certificateProviderFormSchema({ credential_mode: 'access_key' }).safeParse(
      keyValues
    ).success
  ).toBe(true)
  expect(
    certificateProviderFormSchema().safeParse({
      ...keyValues,
      access_key: 'new-key',
    }).success
  ).toBe(true)
})
test('certificate form rejects unsupported mode and whitespace keys', () => {
  expect(
    certificateProviderFormSchema().safeParse({
      ...keyValues,
      provider: 'letsencrypt',
      access_key: 'key',
    }).success
  ).toBe(false)
  expect(
    certificateProviderFormSchema().safeParse({
      ...keyValues,
      access_key: ' key ',
    }).success
  ).toBe(false)
  expect(
    certificateProviderFormSchema().safeParse({
      ...keyValues,
      provider: 'letsencrypt',
      credential_mode: 'email',
      account_email: 'test@example.com',
    }).success
  ).toBe(true)
})

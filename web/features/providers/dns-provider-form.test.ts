import { expect, test } from 'vitest'
import { dnsProviderFormSchema } from './dns-provider-form'

const values = {
  name: 'DNS',
  provider: 'cloudflare',
  account_id: 'account-id',
  api_token: '',
}

test('DNS credentials require 16 to 256 characters except when retaining an existing token', () => {
  for (const editing of [false, true]) {
    for (const length of [0, 15, 16, 256, 257]) {
      expect(
        dnsProviderFormSchema(editing).safeParse({
          ...values,
          api_token: 'a'.repeat(length),
        }).success
      ).toBe((editing && length === 0) || (length >= 16 && length <= 256))
    }
  }
})

test('DNS account identifiers reject embedded whitespace', () => {
  expect(
    dnsProviderFormSchema(true).safeParse({
      ...values,
      account_id: 'account id',
    }).success
  ).toBe(false)
  expect(dnsProviderFormSchema(true).safeParse(values).success).toBe(true)
})

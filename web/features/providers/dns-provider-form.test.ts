import { expect, test } from 'vitest'
import { dnsProviderFormSchema } from './dns-provider-form'

const cloudflare = {
  name: 'DNS',
  provider: 'cloudflare',
  account_id: '0123456789abcdef0123456789ABCDEF',
  api_token: '',
}
const aliyun = { ...cloudflare, provider: 'aliyun', account_id: 'LTAI-account' }

test('Aliyun secrets require 16 to 256 characters except when retaining an existing secret', () => {
  for (const editing of [false, true]) {
    for (const length of [0, 15, 16, 256, 257]) {
      expect(
        dnsProviderFormSchema(editing).safeParse({
          ...aliyun,
          api_token: 'a'.repeat(length),
        }).success
      ).toBe((editing && length === 0) || (length >= 16 && length <= 256))
    }
  }
})

test('Cloudflare tokens require 20 to 200 characters like the backend', () => {
  for (const length of [19, 20, 200, 201]) {
    expect(
      dnsProviderFormSchema(false).safeParse({
        ...cloudflare,
        api_token: 'a'.repeat(length),
      }).success
    ).toBe(length >= 20 && length <= 200)
  }
})

test('Cloudflare account ids must be 32 hexadecimal characters', () => {
  const token = 'a'.repeat(40)
  for (const [accountId, valid] of [
    ['0123456789abcdef0123456789ABCDEF', true],
    ['qa-account-0001', false],
    ['0123456789abcdef0123456789abcdeg', false],
  ] as const)
    expect(
      dnsProviderFormSchema(false).safeParse({
        ...cloudflare,
        account_id: accountId,
        api_token: token,
      }).success
    ).toBe(valid)
})

test('DNS account identifiers reject embedded whitespace', () => {
  expect(
    dnsProviderFormSchema(true).safeParse({
      ...aliyun,
      account_id: 'account id',
    }).success
  ).toBe(false)
  expect(dnsProviderFormSchema(true).safeParse(aliyun).success).toBe(true)
})

import { expect, test } from 'vitest'
import { nodeFormSchema } from './node-form'

const id = '29b9df01-e8bb-4f33-87e5-dfef34c77f8f'
const endpoint = { id, ip_address: '192.0.2.1', line_code: 'default' }
const values = {
  cluster_id: id,
  name: 'node',
  status: 'enabled',
  endpoints: [endpoint],
}
test('node addresses accept IP literals and scoped IPv6 but reject hostnames and malformed addresses', () => {
  for (const ip_address of [
    '192.0.2.1',
    '::1',
    '2001:db8::1',
    '::ffff:192.0.2.1',
    'fe80::1%eth0',
  ])
    expect(
      nodeFormSchema.safeParse({
        ...values,
        endpoints: [{ ...endpoint, ip_address }],
      }).success
    ).toBe(true)
  for (const ip_address of [
    'example.com',
    '256.0.0.1',
    '192.0.2.1:443',
    '2001:::1',
    '192.0.2.1%eth0',
  ])
    expect(
      nodeFormSchema.safeParse({
        ...values,
        endpoints: [{ ...endpoint, ip_address }],
      }).success
    ).toBe(false)
})
test('node endpoints reject duplicate ids or addresses at the offending row', () => {
  const duplicate = nodeFormSchema.safeParse({
    ...values,
    endpoints: [
      endpoint,
      { ...endpoint, id: '39b9df01-e8bb-4f33-87e5-dfef34c77f8f' },
    ],
  })
  expect(duplicate.success).toBe(false)
  if (!duplicate.success)
    expect(duplicate.error.issues[0].path).toEqual([
      'endpoints',
      1,
      'ip_address',
    ])
  expect(
    nodeFormSchema.safeParse({
      ...values,
      endpoints: [endpoint, { ...endpoint, ip_address: '192.0.2.2' }],
    }).success
  ).toBe(false)
})

test('node cache resources enforce byte bounds and relative directory components', () => {
  const cache = {
    memory_bytes: 0,
    disk_bytes: 0,
    max_object_bytes: 1,
    directory: 'cache/v1',
  }
  expect(nodeFormSchema.safeParse({ ...values, cache }).success).toBe(true)
  expect(
    nodeFormSchema.safeParse({
      ...values,
      cache: {
        ...cache,
        memory_bytes: 1099511627776,
        disk_bytes: 1125899906842624,
        max_object_bytes: 1099511627776,
      },
    }).success
  ).toBe(true)
  for (const directory of [
    '',
    '/',
    '/cache',
    'cache/',
    'cache//v1',
    'cache/./v1',
    'cache/../v1',
    'cache\\v1',
    'C:cache',
    'cache ',
    'cache/.',
    'cache/\u0001v1',
  ])
    expect(
      nodeFormSchema.safeParse({ ...values, cache: { ...cache, directory } })
        .success
    ).toBe(false)
  expect(
    nodeFormSchema.safeParse({
      ...values,
      cache: { ...cache, max_object_bytes: 0 },
    }).success
  ).toBe(false)
  expect(
    nodeFormSchema.safeParse({
      ...values,
      cache: { ...cache, memory_bytes: 1099511627777 },
    }).success
  ).toBe(false)
  expect(
    nodeFormSchema.safeParse({
      ...values,
      cache: { ...cache, disk_bytes: 1125899906842625 },
    }).success
  ).toBe(false)
})

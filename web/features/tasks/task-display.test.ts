import { expect, test } from 'vitest'
import { tasksSearchSchema } from './search'
import { taskHref, taskResourceHref, taskTitle } from './task-display'

test('task links open the detail sheet and resource detail routes', () => {
  expect(taskHref({ id: 'a:b', resource_id: 'r1', version: 3 })).toBe(
    '/tasks?task=a%3Ab&resource_id=r1&version=3'
  )
  expect(taskHref({ id: 't', resource_id: 'r1' })).toBe(
    '/tasks?task=t&resource_id=r1'
  )
  const base = { resource_id: 'id-1', name: 'n' }
  expect(taskResourceHref({ ...base, resource_type: 'website' })).toBe(
    '/websites/id-1'
  )
  expect(taskResourceHref({ ...base, resource_type: 'dns_zone' })).toBe(
    '/dns-zones/id-1'
  )
  expect(
    taskResourceHref({ ...base, resource_type: 'provider', name: 'zerossl' })
  ).toBe('/providers/certificate?q=zerossl')
  expect(
    taskResourceHref({ ...base, resource_type: 'provider', name: 'CF 账号' })
  ).toBe('/providers/dns?q=CF+%E8%B4%A6%E5%8F%B7')
  expect(taskTitle({ resource_type: 'certificate', operation: 'renew' })).toBe(
    '证书续期'
  )
  expect(taskTitle({ resource_type: 'unknown', operation: 'x' })).toBe(
    'unknown'
  )
})

test('search schema keeps valid sheet state and drops invalid filters', () => {
  expect(
    tasksSearchSchema.parse({
      task: 123,
      resource_id: 'r',
      version: '4',
      status: 'bogus',
      days: 3,
      tab: 'history',
    })
  ).toEqual({ task: '123', resource_id: 'r', version: 4, tab: 'history' })
})

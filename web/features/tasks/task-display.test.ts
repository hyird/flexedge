import { expect, test } from 'vitest'
import { tasksSearchSchema } from './search'
import { taskHref, taskResourceTarget, taskTitle } from './task-display'

test('task links open the detail sheet and resource detail routes', () => {
  expect(taskHref({ id: 'a:b', resource_id: 'r1', version: 3 })).toBe(
    '/tasks?task=a%3Ab&resource_id=r1&version=3'
  )
  expect(taskHref({ id: 't', resource_id: 'r1' })).toBe(
    '/tasks?task=t&resource_id=r1'
  )
  const base = { resource_id: 'id-1', name: 'n' }
  expect(taskResourceTarget({ ...base, resource_type: 'website' })).toEqual({
    href: '/websites/id-1',
  })
  expect(taskResourceTarget({ ...base, resource_type: 'dns_zone' })).toEqual({
    href: '/dns-zones/id-1',
  })
})

test('provider tasks open the provider tab through history state, not the URL', () => {
  const base = { resource_id: 'id-1', resource_type: 'provider' }
  expect(taskResourceTarget({ ...base, name: 'zerossl' })).toEqual({
    href: '/providers',
    state: { providers: { tab: 'certificate', q: 'zerossl' } },
  })
  expect(taskResourceTarget({ ...base, name: 'CF 账号' })).toEqual({
    href: '/providers',
    state: { providers: { tab: 'dns', q: 'CF 账号' } },
  })
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
  ).toEqual({ task: '123', resource_id: 'r', version: 4 })
})

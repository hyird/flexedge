import { isValidElement } from 'react'
import { toast } from '@heroui/react'
import { afterEach, expect, test, vi } from 'vitest'
import { toastWriteSuccess } from './write-toast'

afterEach(() => {
  vi.restoreAllMocks()
})

test('the success toast links 「查看任务」 to the resource task', () => {
  const success = vi.spyOn(toast, 'success').mockReturnValue('toast-1')
  toastWriteSuccess('配置已保存', {
    resourceType: 'website',
    resourceId: '0d2c6a4e-1f7b-4b8a-9d1e-2a3b4c5d6e7f',
  })
  expect(success).toHaveBeenCalledTimes(1)
  const [message, options] = success.mock.calls[0]
  expect(message).toBe('配置已保存')
  expect(options).toHaveProperty('description')
  const link =
    options && 'description' in options ? options.description : undefined
  expect(isValidElement<{ href: string }>(link)).toBe(true)
  if (!isValidElement<{ href: string }>(link)) return
  expect(link.props.href).toBe(
    '/tasks?task=website%3A0d2c6a4e-1f7b-4b8a-9d1e-2a3b4c5d6e7f'
  )
})

test('writes without a resource task show a plain toast', () => {
  const success = vi.spyOn(toast, 'success').mockReturnValue('toast-1')
  toastWriteSuccess('集群已创建')
  expect(success.mock.calls).toEqual([['集群已创建']])
})

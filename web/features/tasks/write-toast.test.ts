import { isValidElement, type ReactElement } from 'react'
import { toast } from '@heroui/react'
import { renderToStaticMarkup } from 'react-dom/server'
import { afterEach, expect, test, vi } from 'vitest'
import { operationIdOf, toastWriteSuccess } from './write-toast'

afterEach(() => {
  vi.restoreAllMocks()
})

test('write responses name their task only when they carry operation_id', () => {
  expect(operationIdOf({ operation_id: 'op-1' })).toBe('op-1')
  expect(operationIdOf({ id: 'w1', operation_id: 'op-2' })).toBe('op-2')
  for (const data of [
    undefined,
    null,
    'op',
    {},
    { operation_id: '' },
    { operation_id: 7 },
  ])
    expect(operationIdOf(data)).toBeUndefined()
})

test('the success toast offers 「查看任务」 when the write created a task', () => {
  const success = vi.spyOn(toast, 'success').mockReturnValue('toast-1')
  toastWriteSuccess('配置已保存', { operation_id: 'op-9' })
  expect(success).toHaveBeenCalledTimes(1)
  const [message, options] = success.mock.calls[0]
  expect(message).toBe('配置已保存')
  const description = options?.description
  expect(isValidElement(description)).toBe(true)
  const html = renderToStaticMarkup(description as ReactElement)
  expect(html).toContain('查看任务')
  expect(html).toContain('href="/tasks?task=op-9"')

  success.mockClear()
  toastWriteSuccess('集群已创建', undefined)
  toastWriteSuccess('已保存', { id: 'x' })
  expect(success.mock.calls).toEqual([['集群已创建'], ['已保存']])
})

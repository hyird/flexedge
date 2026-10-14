import { z } from 'zod'
import { expect, it } from 'vitest'
import { fieldErrors } from './form-errors'

it('keeps the first issue per top-level field and skips path-less issues', () => {
  const schema = z
    .object({
      account_id: z
        .string()
        .min(8, '至少 8 个字符')
        .regex(/^\S*$/, '不能包含空白字符'),
      records: z.array(z.object({ name: z.string().min(1, '请输入主机记录') })),
    })
    .refine(() => false, '表单不完整')
  const result = schema.safeParse({
    account_id: 'a b',
    records: [{ name: '' }],
  })
  expect(result.success).toBe(false)
  expect(fieldErrors(result.error!.issues)).toEqual({
    account_id: '至少 8 个字符',
    records: '请输入主机记录',
  })
})

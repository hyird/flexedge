import { z } from 'zod'
import { dnsPlatformSchema } from './types'

/** Credential shapes the backend accepts (`dns_provider_service.h`):
 * Cloudflare Account ID is 32 hex characters and its API Token 20–200
 * characters; an Aliyun AccessKey Secret is 16–256 characters. */
export const dnsProviderTokenLength = {
  cloudflare: { min: 20, max: 200, message: 'API Token 需为 20–200 个字符' },
  aliyun: { min: 16, max: 256, message: 'AccessKey Secret 需为 16–256 个字符' },
} as const

const dnsProviderFormObject = z.object({
  name: z.string().trim().min(1, '请输入账号名称').max(100),
  provider: dnsPlatformSchema,
  account_id: z
    .string()
    .trim()
    .min(8, '账户标识至少 8 个字符')
    .max(128)
    .regex(/^\S+$/, '账户标识不能包含空白字符'),
  api_token: z.string(),
})

export type DnsProviderFormValues = z.infer<typeof dnsProviderFormObject>

export function dnsProviderFormSchema(editing = false) {
  return dnsProviderFormObject.superRefine((values, ctx) => {
    if (
      values.provider === 'cloudflare' &&
      !/^[0-9a-fA-F]{32}$/.test(values.account_id)
    )
      ctx.addIssue({
        code: 'custom',
        path: ['account_id'],
        message: 'Cloudflare Account ID 为 32 位十六进制字符',
      })
    if (editing && values.api_token === '') return
    const limit = dnsProviderTokenLength[values.provider]
    if (
      values.api_token.length < limit.min ||
      values.api_token.length > limit.max
    )
      ctx.addIssue({
        code: 'custom',
        path: ['api_token'],
        message: limit.message,
      })
  })
}

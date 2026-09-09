import { z } from 'zod'

export function dnsProviderFormSchema(editing = false) {
  return z.object({
    name: z.string().trim().min(1, '请输入账号名称').max(100),
    provider: z.enum(['cloudflare', 'aliyun']),
    account_id: z
      .string()
      .trim()
      .min(8, '账户标识至少 8 个字符')
      .max(128)
      .regex(/^\S+$/, '账户标识不能包含空白字符'),
    api_token: z
      .string()
      .max(256)
      .refine(
        (token) => (editing && token === '') || token.length >= 16,
        '访问密钥至少 16 个字符'
      ),
  })
}

export type DnsProviderFormValues = z.infer<
  ReturnType<typeof dnsProviderFormSchema>
>

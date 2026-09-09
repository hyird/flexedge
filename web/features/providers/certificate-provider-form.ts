import { z } from 'zod'
import type { CertificateProvider } from './types'

export function certificateProviderFormSchema(
  current?: Pick<CertificateProvider, 'credential_mode'>
) {
  return z
    .object({
      provider: z.enum(['letsencrypt', 'zerossl']),
      credential_mode: z.enum(['email', 'access_key']),
      account_email: z.string().max(254),
      access_key: z.string().max(255),
    })
    .superRefine((value, ctx) => {
      if (value.credential_mode === 'email') {
        if (!z.email().safeParse(value.account_email).success)
          ctx.addIssue({
            code: 'custom',
            path: ['account_email'],
            message: '请输入有效邮箱',
          })
        return
      }
      if (value.provider === 'letsencrypt')
        ctx.addIssue({
          code: 'custom',
          path: ['credential_mode'],
          message: "Let's Encrypt 只支持邮箱接入方式",
        })
      if (!value.access_key && current?.credential_mode !== 'access_key')
        ctx.addIssue({
          code: 'custom',
          path: ['access_key'],
          message: '请输入 API Access Key',
        })
      else if (/\s/.test(value.access_key))
        ctx.addIssue({
          code: 'custom',
          path: ['access_key'],
          message: 'API Access Key 不能包含空白字符',
        })
    })
}
export type CertificateProviderFormValues = z.infer<
  ReturnType<typeof certificateProviderFormSchema>
>

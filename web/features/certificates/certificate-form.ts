import { z } from 'zod'

export function certificateFormSchema(editing = false) {
  return z.object({
    domain: editing
      ? z.string()
      : z
          .string()
          .trim()
          .min(1, '请输入证书域名')
          .max(253)
          .regex(
            /^(?:\*\.)?([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$/,
            '域名格式不正确'
          ),
    certificate_provider_id: editing
      ? z.string()
      : z.string().uuid('请选择证书供应商'),
    dns_zone_id: editing ? z.string() : z.string().uuid('请选择托管域名'),
    auto_renew: z.boolean(),
  })
}
export type CertificateFormValues = z.infer<
  ReturnType<typeof certificateFormSchema>
>

import { z } from 'zod';
import { UUID_PATTERN } from '@/utils/validation';

export const DOMAIN_MAX_LENGTH = 253;
export const DOMAIN_PATTERN =
    /^(?:\*\.)?([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$/;

export const certificateCreateSchema = z.object({
    domain: z
        .string()
        .min(1, '证书域名不能为空')
        .max(DOMAIN_MAX_LENGTH, '证书域名最多253个字符')
        .regex(DOMAIN_PATTERN, '证书域名格式不正确'),
    certificate_provider_id: z
        .string()
        .min(1, '请选择证书供应商')
        .regex(UUID_PATTERN, '证书供应商不正确'),
    dns_zone_id: z.string().min(1, '请选择托管域名').regex(UUID_PATTERN, '托管域名不正确'),
    auto_renew: z.boolean({ error: '自动续签设置不能为空' }),
});

export type CertificateCreateValues = z.infer<typeof certificateCreateSchema>;

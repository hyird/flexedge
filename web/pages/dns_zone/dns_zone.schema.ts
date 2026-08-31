import { z } from 'zod';

export const DOMAIN_MAX_LENGTH = 253;
export const CONTENT_MAX_LENGTH = 4096;
export const DOMAIN_PATTERN = /^([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$/;

export const dnsZoneCreateSchema = z.object({
    dns_provider_id: z.string().uuid('请选择 DNS 服务商账号'),
    domain: z
        .string()
        .trim()
        .min(1, '域名不能为空')
        .max(DOMAIN_MAX_LENGTH, '域名最多253个字符')
        .regex(DOMAIN_PATTERN, '域名格式不正确'),
});

export const dnsRecordSchema = z
    .object({
        type: z.enum(['A', 'AAAA', 'CNAME', 'TXT', 'MX'], { error: '记录类型不支持' }),
        name: z
            .string()
            .trim()
            .min(1, '主机记录不能为空')
            .max(DOMAIN_MAX_LENGTH, '主机记录最多253个字符'),
        content: z
            .string()
            .trim()
            .min(1, '记录值不能为空')
            .max(CONTENT_MAX_LENGTH, '记录值最多4096个字符'),
        ttl: z.number().int().min(1, 'TTL 不正确').max(86400, 'TTL 不正确'),
        priority: z.number().int().min(0).max(65535).optional(),
        proxied: z.boolean(),
        line_code: z.string().min(1, '请选择 DNS 线路'),
    })
    .superRefine((value, context) => {
        if (value.type === 'MX' && value.priority === undefined) {
            context.addIssue({
                code: 'custom',
                path: ['priority'],
                message: 'MX 记录必须填写优先级',
            });
        }
    });

export type DnsZoneCreateValues = z.infer<typeof dnsZoneCreateSchema>;
export type DnsRecordValues = z.infer<typeof dnsRecordSchema>;

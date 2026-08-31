import { z } from 'zod';
import { DNS_PROVIDER_REGISTRY } from '@/config/providers';

export const NAME_MAX_LENGTH = 100;
export const CLOUDFLARE_ACCOUNT_ID_LENGTH = 32;
export const ACCOUNT_ID_MAX_LENGTH = 128;
export const API_TOKEN_MIN_LENGTH = 16;
export const API_TOKEN_MAX_LENGTH = 256;
export const CLOUDFLARE_ACCOUNT_ID_PATTERN = /^[a-fA-F0-9]{32}$/;

export function dnsProviderFormSchema(requireSecret: boolean) {
    return z
        .object({
            name: z
                .string()
                .min(1, '账号名称不能为空')
                .refine((value) => value.trim().length > 0, '账号名称不能为空')
                .max(NAME_MAX_LENGTH, '账号名称最多100个字符'),
            provider: z.enum(['cloudflare', 'aliyun'], {
                error: '请选择服务商类型',
            }),
            account_id: z.string(),
            api_token: z.string(),
        })
        .superRefine((values, context) => {
            if (values.provider === 'cloudflare') {
                if (!values.account_id) {
                    context.addIssue({
                        code: 'custom',
                        path: ['account_id'],
                        message: '账户 ID 不能为空',
                    });
                } else if (!CLOUDFLARE_ACCOUNT_ID_PATTERN.test(values.account_id)) {
                    context.addIssue({
                        code: 'custom',
                        path: ['account_id'],
                        message: '账户 ID 格式不正确',
                    });
                }
            } else if (
                !values.account_id ||
                values.account_id.length < 8 ||
                values.account_id.length > ACCOUNT_ID_MAX_LENGTH ||
                !/^\S+$/.test(values.account_id)
            ) {
                context.addIssue({
                    code: 'custom',
                    path: ['account_id'],
                    message: values.account_id
                        ? 'AccessKey ID 格式不正确'
                        : 'AccessKey ID 不能为空',
                });
            }

            const secretLabel = DNS_PROVIDER_REGISTRY[values.provider].secretLabel;
            if (requireSecret && !values.api_token) {
                context.addIssue({
                    code: 'custom',
                    path: ['api_token'],
                    message: `${secretLabel} 不能为空`,
                });
            } else if (
                values.api_token &&
                (values.api_token.length < API_TOKEN_MIN_LENGTH ||
                    values.api_token.length > API_TOKEN_MAX_LENGTH)
            ) {
                context.addIssue({
                    code: 'custom',
                    path: ['api_token'],
                    message: `${secretLabel} 格式不正确`,
                });
            }
        });
}

export type DnsProviderFormValues = z.infer<ReturnType<typeof dnsProviderFormSchema>>;

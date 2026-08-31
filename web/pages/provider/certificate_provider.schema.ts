import { z } from 'zod';
import { CERTIFICATE_PROVIDER_REGISTRY } from '@/config/providers';

export const ACCOUNT_EMAIL_MAX_LENGTH = 254;
export const ACCESS_KEY_MAX_LENGTH = 255;
export const ACCOUNT_EMAIL_PATTERN =
    /^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$/;
export const ACCESS_KEY_PATTERN = /^\S+$/;

export function certificateProviderFormSchema(requireAccessKey: boolean) {
    return z
        .object({
            provider: z.enum(['letsencrypt', 'zerossl'], {
                error: '请选择供应商类型',
            }),
            credential_mode: z.enum(['email', 'access_key'], {
                error: '请选择接入方式',
            }),
            account_email: z.string(),
            access_key: z.string(),
        })
        .superRefine((values, context) => {
            const supportedModes = CERTIFICATE_PROVIDER_REGISTRY[values.provider]
                .credentialModes as readonly string[];
            if (!supportedModes.includes(values.credential_mode)) {
                context.addIssue({
                    code: 'custom',
                    path: ['credential_mode'],
                    message: '当前供应商不支持此接入方式',
                });
            }

            if (values.credential_mode === 'email') {
                if (!values.account_email) {
                    context.addIssue({
                        code: 'custom',
                        path: ['account_email'],
                        message: '账户邮箱不能为空',
                    });
                } else if (values.account_email.length > ACCOUNT_EMAIL_MAX_LENGTH) {
                    context.addIssue({
                        code: 'custom',
                        path: ['account_email'],
                        message: '账户邮箱最多254个字符',
                    });
                } else if (!ACCOUNT_EMAIL_PATTERN.test(values.account_email)) {
                    context.addIssue({
                        code: 'custom',
                        path: ['account_email'],
                        message: '账户邮箱格式不正确',
                    });
                }
            }

            if (values.credential_mode === 'access_key') {
                if (requireAccessKey && !values.access_key) {
                    context.addIssue({
                        code: 'custom',
                        path: ['access_key'],
                        message: 'ZeroSSL API Access Key 不能为空',
                    });
                } else if (
                    values.access_key &&
                    (values.access_key.length > ACCESS_KEY_MAX_LENGTH ||
                        !ACCESS_KEY_PATTERN.test(values.access_key))
                ) {
                    context.addIssue({
                        code: 'custom',
                        path: ['access_key'],
                        message:
                            values.access_key.length > ACCESS_KEY_MAX_LENGTH
                                ? 'API Access Key 最多255个字符'
                                : 'API Access Key 格式不正确',
                    });
                }
            }
        });
}

export type CertificateProviderFormValues = z.infer<
    ReturnType<typeof certificateProviderFormSchema>
>;

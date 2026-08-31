export const DNS_PROVIDER_REGISTRY = {
    cloudflare: {
        label: 'Cloudflare',
        accountLabel: '账户 ID',
        accountPlaceholder: '32 位 Cloudflare Account ID',
        secretLabel: 'API Token',
        supportsRoutingLines: false,
        supportsProxy: true,
    },
    aliyun: {
        label: '阿里云 DNS',
        accountLabel: 'AccessKey ID',
        accountPlaceholder: '阿里云 RAM 用户 AccessKey ID',
        secretLabel: 'AccessKey Secret',
        supportsRoutingLines: true,
        supportsProxy: false,
    },
} as const;

export type DnsProviderCode = keyof typeof DNS_PROVIDER_REGISTRY;

export const DNS_PROVIDER_OPTIONS = Object.entries(DNS_PROVIDER_REGISTRY).map(
    ([value, provider]) => ({ value: value as DnsProviderCode, label: provider.label })
);

export const CERTIFICATE_PROVIDER_REGISTRY = {
    letsencrypt: {
        label: "Let's Encrypt",
        credentialModes: ['email'],
    },
    zerossl: {
        label: 'ZeroSSL',
        credentialModes: ['email', 'access_key'],
    },
} as const;

export type CertificateProviderCode = keyof typeof CERTIFICATE_PROVIDER_REGISTRY;

export const CERTIFICATE_PROVIDER_OPTIONS = Object.entries(CERTIFICATE_PROVIDER_REGISTRY).map(
    ([value, provider]) => ({ value: value as CertificateProviderCode, label: provider.label })
);

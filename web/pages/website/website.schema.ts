import { z } from 'zod';
import { isIpv4Address } from '@/utils/ip_address';

export const WEBSITE_HOSTNAME_MAX_LENGTH = 253;
export const WEBSITE_NAME_MAX_LENGTH = 100;
export const WEBSITE_HOSTNAME_PATTERN =
    /^(?:\*\.)?([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$/;
const WEBSITE_ORIGIN_HOSTNAME_PATTERN =
    /^([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$/;
export const WEBSITE_ORIGIN_HOST_HEADER_PATTERN =
    /^(?:\$host|([A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63})$/;
const WEBSITE_SUBDOMAIN_PREFIX_PATTERN =
    /^(?:\*|[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*)$/;
const WEBSITE_MIME_TYPE_PATTERN =
    /^[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]*\/(?:\*|[A-Za-z0-9][A-Za-z0-9!#$&^_.+-]*)$/;
const WEBSITE_EXTENSION_PATTERN = /^\.[^\s]{1,126}$/;
const routePathSchema = z
    .string()
    .min(1, '路径不能为空')
    .max(2048, '路径最多2048个字符')
    .refine((value) => value.startsWith('/') && !/[\r\n]/.test(value), '路径必须以 / 开头');
const originGroupSchema = z
    .string()
    .trim()
    .min(1, '源站组不能为空')
    .max(100, '源站组最多100个字符')
    .refine(
        (value) =>
            Array.from(value).every((character) => {
                const code = character.charCodeAt(0);
                return code > 31 && code !== 127;
            }),
        '源站组名称不正确'
    );
const routeMethods = [
    'GET',
    'HEAD',
    'POST',
    'PUT',
    'PATCH',
    'DELETE',
    'OPTIONS',
    'CONNECT',
] as const;
const forbiddenRouteHeaders = new Set([
    'connection',
    'content-length',
    'host',
    'keep-alive',
    'proxy-authenticate',
    'proxy-authorization',
    'te',
    'trailer',
    'transfer-encoding',
    'upgrade',
]);
const routeHeadersTextSchema = z
    .string()
    .max(16384, '请求头配置过长')
    .superRefine((value, context) => {
        const names = new Set<string>();
        value
            .split('\n')
            .map((line) => line.trim())
            .filter(Boolean)
            .forEach((line, index) => {
                const separator = line.indexOf(':');
                const name = separator < 0 ? '' : line.slice(0, separator).trim();
                const headerValue = separator < 0 ? '' : line.slice(separator + 1).trim();
                const normalized = name.toLowerCase();
                if (
                    !name ||
                    !/^[!#$%&'*+.^_`|~0-9A-Za-z-]+$/.test(name) ||
                    forbiddenRouteHeaders.has(normalized) ||
                    /[\r\n]/.test(headerValue)
                ) {
                    context.addIssue({
                        code: 'custom',
                        path: [index],
                        message: `第 ${index + 1} 行请求头不正确`,
                    });
                } else if (names.has(normalized)) {
                    context.addIssue({
                        code: 'custom',
                        path: [index],
                        message: `请求头 ${name} 重复`,
                    });
                }
                names.add(normalized);
            });
    });

const metadataShape = {
    name: z
        .string()
        .trim()
        .min(1, '网站名称不能为空')
        .max(WEBSITE_NAME_MAX_LENGTH, '网站名称最多100个字符'),
};

const domainShape = {
    hostname: z.string(),
    dns_mode: z.enum(['managed', 'external'], { error: '请选择解析方式' }),
    managed_dns_zone_id: z.string().optional(),
    subdomain_prefix: z.string().optional(),
};

function validateDomain(
    data: {
        hostname: string;
        dns_mode: 'managed' | 'external';
        managed_dns_zone_id?: string;
        subdomain_prefix?: string;
    },
    context: z.RefinementCtx
) {
    if (data.dns_mode === 'managed') {
        if (!data.managed_dns_zone_id) {
            context.addIssue({
                code: 'custom',
                path: ['managed_dns_zone_id'],
                message: '请选择 DNS 托管域名',
            });
        }
        const prefix = data.subdomain_prefix?.trim() ?? '';
        if (!prefix) {
            context.addIssue({
                code: 'custom',
                path: ['subdomain_prefix'],
                message: '请输入子域名',
            });
        } else if (!WEBSITE_SUBDOMAIN_PREFIX_PATTERN.test(prefix)) {
            context.addIssue({
                code: 'custom',
                path: ['subdomain_prefix'],
                message: '请输入子域名，例如 www、api.dev 或 *',
            });
        }
        return;
    }

    const hostname = data.hostname.trim();
    if (!hostname) {
        context.addIssue({ code: 'custom', path: ['hostname'], message: '域名不能为空' });
    } else if (hostname.length > WEBSITE_HOSTNAME_MAX_LENGTH) {
        context.addIssue({
            code: 'custom',
            path: ['hostname'],
            message: '域名最多253个字符',
        });
    } else if (!WEBSITE_HOSTNAME_PATTERN.test(hostname)) {
        context.addIssue({ code: 'custom', path: ['hostname'], message: '域名格式不正确' });
    }
}

const originHostSchema = z
    .string()
    .trim()
    .min(1, '源站地址不能为空')
    .max(WEBSITE_HOSTNAME_MAX_LENGTH, '源站地址最多253个字符')
    .refine(
        (value) => isIpv4Address(value) || WEBSITE_ORIGIN_HOSTNAME_PATTERN.test(value),
        '请输入 IPv4 地址或域名'
    );

const originHostHeaderSchema = z
    .string()
    .trim()
    .max(WEBSITE_HOSTNAME_MAX_LENGTH, '回源 Host 最多253个字符')
    .refine((value) => !value || WEBSITE_ORIGIN_HOST_HEADER_PATTERN.test(value), '请输入有效域名');

const originShape = {
    group: originGroupSchema,
    protocol: z.enum(['http', 'https'], { error: '请选择源站协议' }),
    host: originHostSchema,
    port: z
        .number({ error: '源站端口不能为空' })
        .int('源站端口必须是整数')
        .min(1, '源站端口必须在1到65535之间')
        .max(65535, '源站端口必须在1到65535之间'),
    role: z.enum(['primary', 'backup'], { error: '请选择源站角色' }),
    weight: z
        .number({ error: '权重不能为空' })
        .int('权重必须是整数')
        .min(1, '权重必须在1到100之间')
        .max(100, '权重必须在1到100之间'),
    status: z.enum(['enabled', 'disabled'], { error: '请选择源站状态' }),
};

const originSettingsShape = {
    default_origin_group: originGroupSchema,
    origin_host_header: originHostHeaderSchema,
    origin_connect_timeout_seconds: z
        .number({ error: '连接超时不能为空' })
        .int('连接超时必须是整数')
        .min(1, '连接超时必须在1到300秒之间')
        .max(300, '连接超时必须在1到300秒之间'),
    origin_read_timeout_seconds: z
        .number({ error: '读取超时不能为空' })
        .int('读取超时必须是整数')
        .min(1, '读取超时必须在1到600秒之间')
        .max(600, '读取超时必须在1到600秒之间'),
    pass_client_ip: z.boolean(),
    health_check_enabled: z.boolean(),
    health_check_path: routePathSchema,
    health_check_interval_seconds: z
        .number({ error: '检查间隔不能为空' })
        .int('检查间隔必须是整数')
        .min(1, '检查间隔必须在1到3600秒之间')
        .max(3600, '检查间隔必须在1到3600秒之间'),
    health_check_timeout_seconds: z
        .number({ error: '检查超时不能为空' })
        .int('检查超时必须是整数')
        .min(1, '检查超时必须在1到300秒之间')
        .max(300, '检查超时必须在1到300秒之间'),
    health_check_expected_status: z
        .number({ error: '期望状态码不能为空' })
        .int('期望状态码必须是整数')
        .min(100, '期望状态码必须在100到599之间')
        .max(599, '期望状态码必须在100到599之间'),
    healthy_threshold: z
        .number({ error: '健康阈值不能为空' })
        .int('健康阈值必须是整数')
        .min(1, '健康阈值必须在1到10之间')
        .max(10, '健康阈值必须在1到10之间'),
    unhealthy_threshold: z
        .number({ error: '故障阈值不能为空' })
        .int('故障阈值必须是整数')
        .min(1, '故障阈值必须在1到10之间')
        .max(10, '故障阈值必须在1到10之间'),
};

const compressionValueSchema = z
    .string()
    .min(1, '每项不能为空')
    .max(127, '每项最长127字符')
    .refine((value) => !/\s/.test(value), '每项不能包含空白字符');

function uniqueValues(values: string[], context: z.RefinementCtx) {
    if (new Set(values).size !== values.length) {
        context.addIssue({ code: 'custom', message: '配置项不能重复' });
    }
}

const compressionValuesSchema = z
    .array(compressionValueSchema)
    .max(32, '最多配置32项')
    .superRefine(uniqueValues);

const extensionValuesSchema = compressionValuesSchema.superRefine((values, context) => {
    values.forEach((value, index) => {
        if (!WEBSITE_EXTENSION_PATTERN.test(value)) {
            context.addIssue({
                code: 'custom',
                path: [index],
                message: '扩展名必须以 . 开头且不能包含空白字符',
            });
        }
    });
});

const compressionShape = {
    response_compression_enabled: z.boolean(),
    response_compression_min_bytes: z
        .number({ error: '压缩阈值不能为空' })
        .int('压缩阈值必须是整数')
        .min(256, '压缩阈值必须在256字节到1 MiB之间')
        .max(1048576, '压缩阈值必须在256字节到1 MiB之间'),
    response_compression_max_bytes: z
        .number({ error: '最大压缩大小不能为空' })
        .int('最大压缩大小必须是整数')
        .min(0, '最大压缩大小必须在0到64 MiB之间')
        .max(67108864, '最大压缩大小必须在0到64 MiB之间'),
    response_compression_algorithms: z
        .array(z.string())
        .min(1, '请至少选择一种压缩算法')
        .max(3, '请选择1到3种压缩算法')
        .superRefine((values, context) => {
            uniqueValues(values, context);
            values.forEach((value, index) => {
                if (!['zstd', 'br', 'gzip'].includes(value)) {
                    context.addIssue({
                        code: 'custom',
                        path: [index],
                        message: '压缩算法必须是 zstd、br 或 gzip',
                    });
                }
            });
        }),
    response_compression_mime_types: compressionValuesSchema.superRefine((values, context) => {
        values.forEach((value, index) => {
            if (!WEBSITE_MIME_TYPE_PATTERN.test(value)) {
                context.addIssue({
                    code: 'custom',
                    path: [index],
                    message: 'MIME 类型必须是 type/subtype 或 type/*',
                });
            }
        });
    }),
    response_compression_extensions: extensionValuesSchema,
    response_compression_excluded_extensions: extensionValuesSchema,
};

function validateCompression(
    data: {
        response_compression_min_bytes: number;
        response_compression_max_bytes: number;
    },
    context: z.RefinementCtx
) {
    if (
        data.response_compression_max_bytes !== 0 &&
        data.response_compression_max_bytes < data.response_compression_min_bytes
    ) {
        context.addIssue({
            code: 'custom',
            path: ['response_compression_max_bytes'],
            message: '最大压缩大小不能小于最小压缩大小',
        });
    }
}

const httpsShape = {
    https_enabled: z.boolean(),
    certificate_ids: z.array(z.string()).max(20, '最多绑定20张证书'),
    minimum_tls_version: z.enum(['1.2', '1.3']),
    force_https: z.boolean(),
    http2_enabled: z.boolean(),
    hsts_enabled: z.boolean(),
};

function validateHttps(
    data: { https_enabled: boolean; certificate_ids: string[] },
    context: z.RefinementCtx
) {
    if (data.https_enabled && data.certificate_ids.length === 0) {
        context.addIssue({
            code: 'custom',
            path: ['certificate_ids'],
            message: '启用 HTTPS 后请至少选择一张有效证书',
        });
    }
}

const accessLogShape = {
    access_log_enabled: z.boolean(),
    access_log_request_headers: z.boolean(),
    access_log_request_body: z.boolean(),
    access_log_response_headers: z.boolean(),
    access_log_query_params: z.boolean(),
    access_log_cookies: z.boolean(),
    access_log_referer: z.boolean(),
    access_log_user_agent: z.boolean(),
    access_log_status_code_ranges: z
        .array(z.enum(['1xx', '2xx', '3xx', '4xx', '5xx']))
        .min(1, '请至少选择一个状态码范围'),
    access_log_client_abort: z.boolean(),
};

export const websiteMetadataSchema = z.object(metadataShape);
export const websiteDomainSchema = z.object(domainShape).superRefine(validateDomain);
export const websiteOriginSchema = z.object(originShape);
export const websiteOriginSettingsSchema = z.object(originSettingsShape);
export const websiteCompressionSchema = z.object(compressionShape).superRefine(validateCompression);
export const websiteHttpsSchema = z.object(httpsShape).superRefine(validateHttps);
export const websiteAccessLogSchema = z.object(accessLogShape);
export const websiteRouteRuleSchema = z
    .object({
        status: z.enum(['enabled', 'disabled']),
        match_type: z.enum(['exact', 'prefix']),
        path: routePathSchema,
        methods: z.array(z.enum(routeMethods)).max(8, '最多选择8种请求方法'),
        action: z.enum(['proxy', 'redirect']),
        rewrite_path: z.string().max(2048, '重写路径最多2048个字符'),
        redirect_url: z.string().max(2048, '跳转地址最多2048个字符'),
        redirect_status: z.union([z.literal(301), z.literal(302)]),
        origin_group: originGroupSchema.or(z.literal('')),
        request_headers_text: routeHeadersTextSchema,
        response_headers_text: routeHeadersTextSchema,
    })
    .superRefine((value, context) => {
        if (value.action === 'proxy') {
            if (!value.origin_group) {
                context.addIssue({
                    code: 'custom',
                    path: ['origin_group'],
                    message: '请选择源站组',
                });
            }
            if (value.rewrite_path && !value.rewrite_path.startsWith('/')) {
                context.addIssue({
                    code: 'custom',
                    path: ['rewrite_path'],
                    message: '重写路径必须以 / 开头',
                });
            }
        } else if (
            !value.redirect_url ||
            !(value.redirect_url.startsWith('/') || /^https?:\/\//.test(value.redirect_url))
        ) {
            context.addIssue({
                code: 'custom',
                path: ['redirect_url'],
                message: '请输入站内路径或完整 HTTP(S) 地址',
            });
        }
    });

export const websiteCreateSchema = z
    .object({
        ...metadataShape,
        cluster_id: z.string().min(1, '请选择所属集群'),
        ...domainShape,
        origin_protocol: originShape.protocol,
        origin_host: originShape.host,
        origin_port: originShape.port,
        origin_host_header: originHostHeaderSchema,
        ...httpsShape,
        ...compressionShape,
    })
    .superRefine(validateDomain)
    .superRefine(validateHttps)
    .superRefine(validateCompression);

export type WebsiteMetadataValues = z.infer<typeof websiteMetadataSchema>;
export type WebsiteDomainValues = z.infer<typeof websiteDomainSchema>;
export type WebsiteOriginValues = z.infer<typeof websiteOriginSchema>;
export type WebsiteOriginSettingsValues = z.infer<typeof websiteOriginSettingsSchema>;
export type WebsiteCompressionValues = z.infer<typeof websiteCompressionSchema>;
export type WebsiteHttpsValues = z.infer<typeof websiteHttpsSchema>;
export type WebsiteAccessLogValues = z.infer<typeof websiteAccessLogSchema>;
export type WebsiteRouteRuleValues = z.infer<typeof websiteRouteRuleSchema>;
export type WebsiteCreateValues = z.infer<typeof websiteCreateSchema>;

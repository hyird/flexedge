import { zodResolver } from '@hookform/resolvers/zod';
import {
    FileText,
    Gauge,
    Globe2,
    Link2,
    LoaderCircle,
    MoreHorizontal,
    Plus,
    Route,
    Save,
    Server,
    Settings2,
    ShieldCheck,
    Zap,
} from 'lucide-react';
import { type ReactNode, useDeferredValue, useEffect, useMemo, useState } from 'react';
import { Controller, useForm, useWatch } from 'react-hook-form';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { DescriptionList } from '@/components/description_list';
import { EmptyState } from '@/components/empty_state';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Checkbox } from '@/components/ui/checkbox';
import {
    DropdownMenu,
    DropdownMenuContent,
    DropdownMenuItem,
    DropdownMenuTrigger,
} from '@/components/ui/dropdown_menu';
import { Field, FieldDescription, FieldError, FieldLabel } from '@/components/ui/field';
import { Input } from '@/components/ui/input';
import {
    Select,
    SelectContent,
    SelectItem,
    SelectTrigger,
    SelectValue,
} from '@/components/ui/select';
import {
    Sheet,
    SheetContent,
    SheetDescription,
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { Skeleton } from '@/components/ui/skeleton';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { Textarea } from '@/components/ui/textarea';
import { useCertificateList } from '@/pages/certificate/certificate.service';
import DnsZoneSelect, {
    type DnsZoneSelectOption,
} from '@/pages/dns_zone/components/dns_zone_select';
import { formatDateTime } from '@/utils/date';
import { createUuid } from '@/utils/uuid';
import {
    WEBSITE_HOSTNAME_MAX_LENGTH,
    WEBSITE_NAME_MAX_LENGTH,
    type WebsiteAccessLogValues,
    type WebsiteCompressionValues,
    type WebsiteDomainValues,
    type WebsiteHttpsValues,
    type WebsiteMetadataValues,
    type WebsiteOriginSettingsValues,
    type WebsiteOriginValues,
    type WebsiteRouteRuleValues,
    websiteAccessLogSchema,
    websiteCompressionSchema,
    websiteDomainSchema,
    websiteHttpsSchema,
    websiteMetadataSchema,
    websiteOriginSchema,
    websiteOriginSettingsSchema,
    websiteRouteRuleSchema,
} from '../website.schema';
import { useWebsiteDetail, useWebsiteSave } from '../website.service';
import type {
    WebsiteAccessLogStatusCodeRange,
    WebsiteDomainItem,
    WebsiteOriginItem,
    WebsiteRouteRule,
} from '../website.types';
import {
    AlgorithmOrder,
    CertificatePicker,
    ChoiceGroup,
    SectionCard,
    SwitchRow,
    TagEditor,
} from './website_form_controls';
import {
    certificateDomainCoversHostname,
    currentWebsiteInput,
    defaultResponseCompressionMimeTypes,
    resolutionStatusMeta,
    websiteResolution,
    websiteSaveTarget,
    websiteStatusMeta,
} from './website_helpers';

interface WebsiteDetailSheetProps {
    websiteId?: string;
    onClose: () => void;
}

type WebsiteDetailTab =
    | 'basic'
    | 'domains'
    | 'origins'
    | 'routes'
    | 'compression'
    | 'https'
    | 'accessLog';

const detailTabs: Array<{
    value: WebsiteDetailTab;
    label: string;
    icon: ReactNode;
}> = [
    { value: 'basic', label: '基本信息', icon: <Settings2 /> },
    { value: 'domains', label: '域名管理', icon: <Globe2 /> },
    { value: 'origins', label: '源站与回源', icon: <Server /> },
    { value: 'routes', label: '路由规则', icon: <Route /> },
    { value: 'compression', label: '内容压缩', icon: <Zap /> },
    { value: 'https', label: 'HTTPS', icon: <ShieldCheck /> },
    { value: 'accessLog', label: '访问日志', icon: <FileText /> },
];

const accessLogStatusCodeRangeOptions: Array<{
    label: string;
    value: WebsiteAccessLogStatusCodeRange;
}> = [
    { label: '1xx', value: '1xx' },
    { label: '2xx', value: '2xx' },
    { label: '3xx', value: '3xx' },
    { label: '4xx', value: '4xx' },
    { label: '5xx', value: '5xx' },
];

const routeMethodOptions = [
    'GET',
    'HEAD',
    'POST',
    'PUT',
    'PATCH',
    'DELETE',
    'OPTIONS',
    'CONNECT',
] as const;

function parseRouteHeaders(value: string) {
    return value
        .split('\n')
        .map((line) => line.trim())
        .filter(Boolean)
        .map((line) => {
            const separator = line.indexOf(':');
            return {
                name: line.slice(0, separator).trim(),
                value: line.slice(separator + 1).trim(),
            };
        });
}

const responseCompressionMimeTypeOptions = [
    'text/*',
    'text/html',
    'text/css',
    'text/plain',
    'text/javascript',
    'text/xml',
    'text/csv',
    'text/calendar',
    'text/markdown',
    'text/vcard',
    'text/cache-manifest',
    'application/javascript',
    'application/x-javascript',
    'application/ecmascript',
    'application/json',
    'application/ld+json',
    'application/manifest+json',
    'application/x-web-app-manifest+json',
    'application/vnd.api+json',
    'application/geo+json',
    'application/problem+json',
    'application/activity+json',
    'application/feed+json',
    'application/hal+json',
    'application/schema+json',
    'application/json-patch+json',
    'application/merge-patch+json',
    'application/json-seq',
    'application/xml',
    'application/xhtml+xml',
    'application/rss+xml',
    'application/atom+xml',
    'application/mathml+xml',
    'application/xslt+xml',
    'application/sitemap+xml',
    'application/graphql',
    'application/graphql-response+json',
    'application/wasm',
    'image/svg+xml',
    'font/ttf',
    'font/otf',
    'font/collection',
    'font/woff',
    'font/woff2',
    'application/vnd.ms-fontobject',
];

function isDetailTab(value: string): value is WebsiteDetailTab {
    return detailTabs.some((tab) => tab.value === value);
}

function originHostHeaderLabel(value: string) {
    return value === '$host' ? '访问域名' : value;
}

function FormControl({
    id,
    label,
    error,
    description,
    children,
    className,
}: {
    id: string;
    label: ReactNode;
    error?: string;
    description?: ReactNode;
    children: ReactNode;
    className?: string;
}) {
    return (
        <Field data-invalid={Boolean(error)} className={className}>
            <FieldLabel htmlFor={id}>{label}</FieldLabel>
            {children}
            {description && <FieldDescription>{description}</FieldDescription>}
            <FieldError>{error}</FieldError>
        </Field>
    );
}

function SubmitButton({ pending, children }: { pending: boolean; children: ReactNode }) {
    return (
        <Button type="submit" disabled={pending}>
            {pending ? <LoaderCircle className="animate-spin" /> : <Save />}
            {children}
        </Button>
    );
}

function DetailLoading() {
    return (
        <div className="space-y-4 p-5">
            <Skeleton className="h-10 w-full" />
            <Skeleton className="h-48 w-full" />
            <Skeleton className="h-32 w-full" />
        </div>
    );
}

export default function WebsiteDetailSheet({ websiteId, onClose }: WebsiteDetailSheetProps) {
    const [activeTab, setActiveTab] = useState<WebsiteDetailTab>('basic');
    const [certificateKeyword, setCertificateKeyword] = useState('');
    const [knownCertificates, setKnownCertificates] = useState<
        Array<{ id: string; domains: string[]; usable: boolean }>
    >([]);
    const deferredCertificateKeyword = useDeferredValue(certificateKeyword.trim());
    const [addingDomain, setAddingDomain] = useState(false);
    const [domainDnsZone, setDomainDnsZone] = useState<DnsZoneSelectOption>();
    const [addingOrigin, setAddingOrigin] = useState(false);
    const [addingRoute, setAddingRoute] = useState(false);
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const detail = useWebsiteDetail(websiteId, true);
    const save = useWebsiteSave();
    const website = detail.data;
    const certificates = useCertificateList(
        {
            page: 1,
            pageSize: 20,
            usable: true,
            keyword: deferredCertificateKeyword || undefined,
        },
        Boolean(websiteId) && activeTab === 'https'
    );

    const metadataForm = useForm<WebsiteMetadataValues>({
        resolver: zodResolver(websiteMetadataSchema),
        defaultValues: { name: '' },
    });
    const domainForm = useForm<WebsiteDomainValues>({
        resolver: zodResolver(websiteDomainSchema),
        defaultValues: {
            hostname: '',
            dns_mode: 'managed',
            managed_dns_zone_id: undefined,
            subdomain_prefix: '',
        },
    });
    const originForm = useForm<WebsiteOriginValues>({
        resolver: zodResolver(websiteOriginSchema),
        defaultValues: {
            group: 'default',
            protocol: 'http',
            host: '',
            port: 80,
            role: 'primary',
            weight: 10,
            status: 'enabled',
        },
    });
    const originSettingsForm = useForm<WebsiteOriginSettingsValues>({
        resolver: zodResolver(websiteOriginSettingsSchema),
        defaultValues: {
            default_origin_group: 'default',
            origin_host_header: '',
            origin_connect_timeout_seconds: 10,
            origin_read_timeout_seconds: 30,
            pass_client_ip: true,
            health_check_enabled: true,
            health_check_path: '/',
            health_check_interval_seconds: 10,
            health_check_timeout_seconds: 3,
            health_check_expected_status: 200,
            healthy_threshold: 2,
            unhealthy_threshold: 3,
        },
    });
    const routeForm = useForm<WebsiteRouteRuleValues>({
        resolver: zodResolver(websiteRouteRuleSchema),
        defaultValues: {
            status: 'enabled',
            match_type: 'prefix',
            path: '/',
            methods: [],
            action: 'proxy',
            rewrite_path: '',
            redirect_url: '',
            redirect_status: 302,
            origin_group: 'default',
            request_headers_text: '',
            response_headers_text: '',
        },
    });
    const compressionForm = useForm<WebsiteCompressionValues>({
        resolver: zodResolver(websiteCompressionSchema),
        defaultValues: {
            response_compression_enabled: true,
            response_compression_min_bytes: 1024,
            response_compression_max_bytes: 32 * 1024 * 1024,
            response_compression_algorithms: ['zstd', 'br', 'gzip'],
            response_compression_mime_types: [...defaultResponseCompressionMimeTypes],
            response_compression_extensions: [],
            response_compression_excluded_extensions: [],
        },
    });
    const httpsForm = useForm<WebsiteHttpsValues>({
        resolver: zodResolver(websiteHttpsSchema),
        defaultValues: {
            https_enabled: false,
            certificate_ids: [],
            minimum_tls_version: '1.2',
            force_https: true,
            http2_enabled: true,
            hsts_enabled: false,
        },
    });
    const accessLogForm = useForm<WebsiteAccessLogValues>({
        resolver: zodResolver(websiteAccessLogSchema),
        defaultValues: {
            access_log_enabled: true,
            access_log_request_headers: false,
            access_log_request_body: false,
            access_log_response_headers: false,
            access_log_query_params: false,
            access_log_cookies: false,
            access_log_referer: false,
            access_log_user_agent: false,
            access_log_status_code_ranges: ['1xx', '2xx', '3xx', '4xx', '5xx'],
            access_log_client_abort: false,
        },
    });

    const domainDnsMode = useWatch({ control: domainForm.control, name: 'dns_mode' });
    const routeAction = useWatch({ control: routeForm.control, name: 'action' });
    const httpsEnabled = useWatch({ control: httpsForm.control, name: 'https_enabled' });
    const selectedCertificateIds = useWatch({
        control: httpsForm.control,
        name: 'certificate_ids',
    });
    const accessLogEnabled = useWatch({
        control: accessLogForm.control,
        name: 'access_log_enabled',
    });

    const websiteDomains = useMemo<WebsiteDomainItem[]>(
        () =>
            website?.config.domains.map((domain) => {
                const state = website.runtime.domain_states.find((item) => item.id === domain.id);
                return {
                    ...domain,
                    access_protocol: state?.access_protocol ?? 'http',
                    resolution_status: state?.resolution_status ?? 'unverified',
                    last_verified_at: state?.last_verified_at,
                    last_error: state?.last_error,
                };
            }) ?? [],
        [website]
    );
    const websiteOrigins = useMemo<WebsiteOriginItem[]>(
        () => website?.config.origins ?? [],
        [website]
    );
    const selectableCertificates = useMemo(() => {
        const values = new Map(
            (website?.certificates ?? []).map((certificate) => [certificate.id, certificate])
        );
        for (const certificate of knownCertificates) values.set(certificate.id, certificate);
        for (const certificate of certificates.data?.list ?? [])
            values.set(certificate.id, certificate);
        return [...values.values()];
    }, [certificates.data?.list, knownCertificates, website?.certificates]);
    const certificateOptions = useMemo(
        () =>
            selectableCertificates.map((certificate) => ({
                value: String(certificate.id),
                label: certificate.domains.join('、'),
                usable: certificate.usable,
            })),
        [selectableCertificates]
    );
    const uncoveredDomains = useMemo(() => {
        if (!httpsEnabled) return [];
        const selectedIds = new Set(selectedCertificateIds);
        const selectedCertificates = selectableCertificates.filter(
            (certificate) => certificate.usable && selectedIds.has(String(certificate.id))
        );
        return websiteDomains
            .filter(
                (domain) =>
                    !selectedCertificates.some((certificate) =>
                        certificate.domains.some((certificateDomain) =>
                            certificateDomainCoversHostname(certificateDomain, domain.hostname)
                        )
                    )
            )
            .map((domain) => domain.hostname);
    }, [httpsEnabled, selectableCertificates, selectedCertificateIds, websiteDomains]);

    useEffect(() => {
        if (!certificates.data?.list.length) return;
        setKnownCertificates((current) => {
            const values = new Map(current.map((certificate) => [certificate.id, certificate]));
            for (const certificate of certificates.data?.list ?? [])
                values.set(certificate.id, certificate);
            return [...values.values()];
        });
    }, [certificates.data?.list]);

    useEffect(() => {
        if (!website || metadataForm.formState.isDirty) return;
        metadataForm.reset({
            name: website.website_name,
        });
    }, [metadataForm, website]);

    useEffect(() => {
        if (!website || originSettingsForm.formState.isDirty) return;
        originSettingsForm.reset({
            default_origin_group: website.config.default_origin_group,
            origin_host_header:
                website.config.origin_host_header === '$host'
                    ? ''
                    : website.config.origin_host_header,
            origin_connect_timeout_seconds: website.config.origin_connect_timeout_seconds,
            origin_read_timeout_seconds: website.config.origin_read_timeout_seconds,
            pass_client_ip: website.config.pass_client_ip,
            health_check_enabled: website.config.health_check_enabled,
            health_check_path: website.config.health_check_path,
            health_check_interval_seconds: website.config.health_check_interval_seconds,
            health_check_timeout_seconds: website.config.health_check_timeout_seconds,
            health_check_expected_status: website.config.health_check_expected_status,
            healthy_threshold: website.config.healthy_threshold,
            unhealthy_threshold: website.config.unhealthy_threshold,
        });
    }, [originSettingsForm, website]);

    useEffect(() => {
        if (!website || compressionForm.formState.isDirty) return;
        compressionForm.reset({
            response_compression_enabled: website.config.response_compression_enabled,
            response_compression_min_bytes: website.config.response_compression_min_bytes,
            response_compression_max_bytes: website.config.response_compression_max_bytes,
            response_compression_algorithms: website.config.response_compression_algorithms,
            response_compression_mime_types: website.config.response_compression_mime_types,
            response_compression_extensions: website.config.response_compression_extensions,
            response_compression_excluded_extensions:
                website.config.response_compression_excluded_extensions,
        });
    }, [compressionForm, website]);

    useEffect(() => {
        if (!website || httpsForm.formState.isDirty) return;
        httpsForm.reset({
            https_enabled: website.config.https_enabled,
            certificate_ids: website.config.certificate_ids,
            minimum_tls_version: website.config.minimum_tls_version,
            force_https: website.config.force_https,
            http2_enabled: website.config.http2_enabled,
            hsts_enabled: website.config.hsts_enabled,
        });
    }, [httpsForm, website]);

    useEffect(() => {
        if (!website || accessLogForm.formState.isDirty) return;
        accessLogForm.reset({
            access_log_enabled: website.config.access_log_enabled,
            access_log_request_headers: website.config.access_log_request_headers,
            access_log_request_body: website.config.access_log_request_body,
            access_log_response_headers: website.config.access_log_response_headers,
            access_log_query_params: website.config.access_log_query_params,
            access_log_cookies: website.config.access_log_cookies,
            access_log_referer: website.config.access_log_referer,
            access_log_user_agent: website.config.access_log_user_agent,
            access_log_status_code_ranges: website.config.access_log_status_code_ranges,
            access_log_client_abort: website.config.access_log_client_abort,
        });
    }, [accessLogForm, website]);

    const dirtySections: Record<WebsiteDetailTab, boolean> = {
        basic: metadataForm.formState.isDirty,
        domains: addingDomain,
        origins: addingOrigin || originSettingsForm.formState.isDirty,
        routes: addingRoute,
        compression: compressionForm.formState.isDirty,
        https: httpsForm.formState.isDirty,
        accessLog: accessLogForm.formState.isDirty,
    };
    const hasUnsavedChanges = Object.values(dirtySections).some(Boolean);

    const performClose = () => {
        setAddingDomain(false);
        setAddingOrigin(false);
        setAddingRoute(false);
        setDomainDnsZone(undefined);
        setConfirmation(undefined);
        setActiveTab('basic');
        setCertificateKeyword('');
        setKnownCertificates([]);
        domainForm.reset();
        metadataForm.reset();
        originForm.reset();
        originSettingsForm.reset();
        routeForm.reset();
        compressionForm.reset();
        accessLogForm.reset();
        httpsForm.reset();
        onClose();
    };

    const close = () => {
        if (save.isPending) return;
        if (!hasUnsavedChanges) {
            performClose();
            return;
        }
        setConfirmation({
            title: '放弃未保存的修改？',
            content: '当前配置尚未保存，关闭后本次填写的内容将丢失。',
            confirmText: '放弃修改',
            danger: true,
            onConfirm: performClose,
        });
    };

    const domainColumns: Array<DataTableColumn<WebsiteDomainItem>> = [
        {
            key: 'hostname',
            header: '域名',
            cell: (item) => <Badge variant="info">{item.hostname}</Badge>,
        },
        {
            key: 'cname',
            header: 'CNAME 目标',
            cell: () => <span className="font-mono text-xs">{website?.access_domain}</span>,
        },
        {
            key: 'dns-mode',
            header: '解析方式',
            cell: (item) => (item.dns_mode === 'managed' ? '托管解析' : '外部解析'),
        },
        {
            key: 'protocol',
            header: '访问协议',
            cell: (item) => (
                <div className="flex gap-1">
                    <Badge variant="neutral">HTTP</Badge>
                    {item.access_protocol === 'https' && <Badge variant="success">HTTPS</Badge>}
                </div>
            ),
        },
        {
            key: 'resolution',
            header: '解析状态',
            cell: (item) => {
                const meta = resolutionStatusMeta[item.resolution_status];
                return (
                    <div className="max-w-64">
                        <StatusBadge tone={meta.tone}>{meta.label}</StatusBadge>
                        {item.last_error && (
                            <p
                                className="mt-1 truncate text-xs text-destructive"
                                title={item.last_error}
                            >
                                {item.last_error}
                            </p>
                        )}
                    </div>
                );
            },
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (item) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${item.hostname}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem
                            variant="destructive"
                            disabled={websiteDomains.length <= 1 || save.isPending}
                            title={websiteDomains.length <= 1 ? '至少保留一个绑定域名' : undefined}
                            onSelect={() => {
                                if (!website) return;
                                setConfirmation({
                                    title: `解绑域名「${item.hostname}」？`,
                                    danger: true,
                                    confirmText: '解绑',
                                    onConfirm: () =>
                                        save.mutateAsync({
                                            target: websiteSaveTarget(website),
                                            input: {
                                                ...currentWebsiteInput(website),
                                                config: {
                                                    ...website.config,
                                                    domains: website.config.domains.filter(
                                                        (domain) => domain.id !== item.id
                                                    ),
                                                },
                                            },
                                        }),
                                });
                            }}
                        >
                            解绑
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    const originColumns: Array<DataTableColumn<WebsiteOriginItem>> = [
        {
            key: 'origin',
            header: '源站地址',
            cell: (item) => (
                <span className="font-mono text-xs">
                    {item.host}:{item.port}
                </span>
            ),
        },
        {
            key: 'group',
            header: '源站组',
            cell: (item) => <Badge variant="outline">{item.group}</Badge>,
        },
        { key: 'protocol', header: '协议', cell: (item) => item.protocol.toUpperCase() },
        {
            key: 'role',
            header: '角色',
            cell: (item) => (
                <Badge variant={item.role === 'primary' ? 'info' : 'neutral'}>
                    {item.role === 'primary' ? '主源站' : '备用'}
                </Badge>
            ),
        },
        { key: 'weight', header: '权重', cell: (item) => item.weight },
        {
            key: 'status',
            header: '状态',
            cell: (item) => (
                <StatusBadge tone={websiteStatusMeta[item.status].tone}>
                    {websiteStatusMeta[item.status].label}
                </StatusBadge>
            ),
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (item) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${item.host}:${item.port}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem
                            variant="destructive"
                            disabled={websiteOrigins.length <= 1 || save.isPending}
                            title={websiteOrigins.length <= 1 ? '网站至少保留一个源站' : undefined}
                            onSelect={() => {
                                if (!website) return;
                                setConfirmation({
                                    title: `删除源站「${item.host}:${item.port}」？`,
                                    danger: true,
                                    confirmText: '删除',
                                    onConfirm: () =>
                                        save.mutateAsync({
                                            target: websiteSaveTarget(website),
                                            input: {
                                                ...currentWebsiteInput(website),
                                                config: {
                                                    ...website.config,
                                                    origins: website.config.origins.filter(
                                                        (origin) => origin.id !== item.id
                                                    ),
                                                },
                                            },
                                        }),
                                });
                            }}
                        >
                            删除
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    const routeColumns: Array<DataTableColumn<WebsiteRouteRule>> = [
        {
            key: 'match',
            header: '匹配条件',
            cell: (item) => (
                <div>
                    <span className="font-mono text-xs">{item.path}</span>
                    <p className="mt-1 text-xs text-muted-foreground">
                        {item.match_type === 'exact' ? '精确匹配' : '前缀匹配'} ·{' '}
                        {item.methods.length ? item.methods.join(', ') : '全部方法'}
                    </p>
                </div>
            ),
        },
        {
            key: 'action',
            header: '动作',
            cell: (item) => (
                <div>
                    <Badge variant={item.action === 'proxy' ? 'info' : 'warning'}>
                        {item.action === 'proxy' ? '代理' : '跳转'}
                    </Badge>
                    <p className="mt-1 max-w-64 truncate text-xs text-muted-foreground">
                        {item.action === 'proxy'
                            ? `${item.origin_group}${item.rewrite_path ? ` → ${item.rewrite_path}` : ''}`
                            : `${item.redirect_status} → ${item.redirect_url}`}
                    </p>
                </div>
            ),
        },
        {
            key: 'headers',
            header: 'Header 修改',
            cell: (item) =>
                `${item.request_headers.length} 请求 / ${item.response_headers.length} 响应`,
        },
        {
            key: 'status',
            header: '状态',
            cell: (item) => (
                <StatusBadge tone={websiteStatusMeta[item.status].tone}>
                    {websiteStatusMeta[item.status].label}
                </StatusBadge>
            ),
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (item) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${item.path}路由规则操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem
                            variant="destructive"
                            disabled={save.isPending}
                            onSelect={() => {
                                if (!website) return;
                                setConfirmation({
                                    title: `删除路由规则「${item.path}」？`,
                                    danger: true,
                                    confirmText: '删除',
                                    onConfirm: () =>
                                        save.mutateAsync({
                                            target: websiteSaveTarget(website),
                                            input: {
                                                ...currentWebsiteInput(website),
                                                config: {
                                                    ...website.config,
                                                    route_rules: website.config.route_rules.filter(
                                                        (rule) => rule.id !== item.id
                                                    ),
                                                },
                                            },
                                        }),
                                });
                            }}
                        >
                            删除
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    const basicSection = website && (
        <div className="space-y-4">
            <SectionCard title="基本信息" description="用于识别此网站">
                <form
                    noValidate
                    className="space-y-4"
                    onSubmit={metadataForm.handleSubmit((data) =>
                        save.mutate(
                            {
                                target: websiteSaveTarget(website),
                                input: {
                                    ...currentWebsiteInput(website),
                                    config: { ...website.config, name: data.name.trim() },
                                },
                            },
                            { onSuccess: () => metadataForm.reset(data) }
                        )
                    )}
                >
                    <div className="grid gap-4 md:grid-cols-2">
                        <FormControl
                            id="website-name"
                            label="网站名称"
                            error={metadataForm.formState.errors.name?.message}
                        >
                            <Input
                                id="website-name"
                                maxLength={WEBSITE_NAME_MAX_LENGTH}
                                aria-invalid={Boolean(metadataForm.formState.errors.name)}
                                {...metadataForm.register('name')}
                            />
                        </FormControl>
                    </div>
                    <div className="flex justify-end">
                        <SubmitButton pending={save.isPending}>保存基本信息</SubmitButton>
                    </div>
                </form>
            </SectionCard>
            <DescriptionList
                columns={2}
                items={[
                    {
                        label: '启用状态',
                        value: (
                            <StatusBadge tone={websiteStatusMeta[website.status].tone}>
                                {websiteStatusMeta[website.status].label}
                            </StatusBadge>
                        ),
                    },
                    { label: '部署集群', value: website.cluster_name },
                    {
                        label: '接入域名',
                        value: <span className="font-mono text-xs">{website.access_domain}</span>,
                    },
                    { label: '绑定域名', value: `${websiteDomains.length} 个` },
                    {
                        label: '解析方式',
                        value: `${websiteDomains.filter((domain) => domain.dns_mode === 'managed').length} 个托管 / ${websiteDomains.filter((domain) => domain.dns_mode === 'external').length} 个外部`,
                    },
                    {
                        label: '解析状态',
                        value: (() => {
                            const meta = websiteResolution(website);
                            return <StatusBadge tone={meta.tone}>{meta.label}</StatusBadge>;
                        })(),
                    },
                    {
                        label: '访问协议',
                        value: `${websiteDomains.filter((domain) => domain.access_protocol === 'https').length} 个 HTTPS / ${websiteDomains.length} 个 HTTP`,
                    },
                    {
                        label: '源站',
                        value: `${websiteOrigins.filter((origin) => origin.role === 'primary').length} 个主源站 / ${websiteOrigins.filter((origin) => origin.role === 'backup').length} 个备用源站`,
                    },
                    {
                        label: '健康检查',
                        value: website.config.health_check_enabled ? '已启用' : '已关闭',
                    },
                    {
                        label: '回源 Host',
                        value: originHostHeaderLabel(website.config.origin_host_header),
                    },
                    {
                        label: '回源超时',
                        value: `连接 ${website.config.origin_connect_timeout_seconds} 秒 / 读取 ${website.config.origin_read_timeout_seconds} 秒`,
                    },
                    { label: '创建时间', value: formatDateTime(website.created_at) },
                    { label: '更新时间', value: formatDateTime(website.updated_at) },
                ]}
            />
        </div>
    );

    const domainsSection = website && (
        <div className="space-y-4">
            <SectionCard
                title="绑定域名"
                description="支持多个域名，并可由不同证书分别覆盖"
                action={
                    <Button
                        variant={addingDomain ? 'outline' : 'default'}
                        size="sm"
                        onClick={() => {
                            if (addingDomain) {
                                domainForm.reset();
                                setDomainDnsZone(undefined);
                                setAddingDomain(false);
                                return;
                            }
                            domainForm.reset({
                                hostname: '',
                                dns_mode: 'managed',
                                managed_dns_zone_id: undefined,
                                subdomain_prefix: '',
                            });
                            setDomainDnsZone(undefined);
                            setAddingDomain(true);
                        }}
                    >
                        {addingDomain ? (
                            '取消添加'
                        ) : (
                            <>
                                <Plus />
                                添加域名
                            </>
                        )}
                    </Button>
                }
            >
                {addingDomain && (
                    <form
                        noValidate
                        className="mb-5 space-y-4 rounded-lg border bg-muted/20 p-4"
                        onSubmit={domainForm.handleSubmit((data) => {
                            let hostname = data.hostname.toLowerCase();
                            if (data.dns_mode === 'managed') {
                                if (!domainDnsZone) {
                                    domainForm.setError('managed_dns_zone_id', {
                                        message: '请重新选择 DNS 托管域名',
                                    });
                                    return;
                                }
                                hostname =
                                    `${data.subdomain_prefix}.${domainDnsZone.domain}`.toLowerCase();
                            }
                            save.mutate(
                                {
                                    target: websiteSaveTarget(website),
                                    input: {
                                        ...currentWebsiteInput(website),
                                        config: {
                                            ...website.config,
                                            domains: [
                                                ...website.config.domains,
                                                {
                                                    id: createUuid(),
                                                    hostname,
                                                    dns_mode: data.dns_mode,
                                                },
                                            ],
                                        },
                                    },
                                },
                                {
                                    onSuccess: () => {
                                        domainForm.reset();
                                        setDomainDnsZone(undefined);
                                        setAddingDomain(false);
                                    },
                                }
                            );
                        })}
                    >
                        <Controller
                            control={domainForm.control}
                            name="dns_mode"
                            render={({ field }) => (
                                <FormControl
                                    id="domain-dns-mode"
                                    label="解析方式"
                                    error={domainForm.formState.errors.dns_mode?.message}
                                >
                                    <ChoiceGroup
                                        id="domain-dns-mode"
                                        aria-label="解析方式"
                                        value={field.value}
                                        onValueChange={(value) => {
                                            field.onChange(value);
                                            if (value === 'external') {
                                                domainForm.setValue(
                                                    'managed_dns_zone_id',
                                                    undefined,
                                                    { shouldDirty: true }
                                                );
                                                setDomainDnsZone(undefined);
                                            }
                                        }}
                                        options={[
                                            { value: 'managed', label: '托管解析' },
                                            { value: 'external', label: '外部解析' },
                                        ]}
                                    />
                                </FormControl>
                            )}
                        />
                        {domainDnsMode === 'managed' ? (
                            <div className="grid gap-4 md:grid-cols-2">
                                <Controller
                                    control={domainForm.control}
                                    name="managed_dns_zone_id"
                                    render={({ field, fieldState }) => (
                                        <FormControl
                                            id="domain-zone"
                                            label="DNS 托管域名"
                                            error={fieldState.error?.message}
                                        >
                                            <DnsZoneSelect
                                                id="domain-zone"
                                                value={field.value}
                                                onChange={field.onChange}
                                                requireAvailable
                                                invalid={Boolean(fieldState.error)}
                                                onDnsZoneChange={setDomainDnsZone}
                                            />
                                        </FormControl>
                                    )}
                                />
                                <FormControl
                                    id="domain-prefix"
                                    label="子域名"
                                    error={domainForm.formState.errors.subdomain_prefix?.message}
                                    description={
                                        domainDnsZone
                                            ? `完整域名：${domainForm.watch('subdomain_prefix') || 'www'}.${domainDnsZone.domain}`
                                            : undefined
                                    }
                                >
                                    <Input
                                        id="domain-prefix"
                                        placeholder="www"
                                        aria-invalid={Boolean(
                                            domainForm.formState.errors.subdomain_prefix
                                        )}
                                        {...domainForm.register('subdomain_prefix')}
                                    />
                                </FormControl>
                            </div>
                        ) : (
                            <FormControl
                                id="domain-hostname"
                                label="域名"
                                error={domainForm.formState.errors.hostname?.message}
                            >
                                <Input
                                    id="domain-hostname"
                                    maxLength={WEBSITE_HOSTNAME_MAX_LENGTH}
                                    placeholder="static.example.com 或 *.example.com"
                                    aria-invalid={Boolean(domainForm.formState.errors.hostname)}
                                    {...domainForm.register('hostname')}
                                />
                            </FormControl>
                        )}
                        <Button type="submit" disabled={save.isPending}>
                            {save.isPending ? <LoaderCircle className="animate-spin" /> : <Link2 />}
                            绑定域名
                        </Button>
                    </form>
                )}
                {websiteDomains.length > 0 &&
                    websiteDomains.every((domain) => domain.access_protocol === 'http') && (
                        <Alert className="mb-4">
                            <Globe2 />
                            <AlertTitle>当前网站仅提供 HTTP</AlertTitle>
                            <AlertDescription>
                                域名可以直接绑定；需要 HTTPS 时，再为对应域名选择有效证书。
                            </AlertDescription>
                        </Alert>
                    )}
                <DataTable
                    columns={domainColumns}
                    data={websiteDomains}
                    getRowKey={(item) => item.id}
                    emptyTitle="暂无绑定域名"
                />
                <Alert className="mt-4">
                    <Globe2 />
                    <AlertTitle>DNS 配置说明</AlertTitle>
                    <AlertDescription>
                        托管解析由系统自动维护；外部解析请手动 CNAME 到{' '}
                        <span className="font-mono">{website.access_domain}</span>。
                    </AlertDescription>
                </Alert>
            </SectionCard>
        </div>
    );

    const originsSection = website && (
        <div className="space-y-4">
            <SectionCard
                title="源站列表"
                description="主源站按权重分配请求；全部不可用时使用备用源站"
                action={
                    <Button
                        variant={addingOrigin ? 'outline' : 'default'}
                        size="sm"
                        onClick={() => {
                            if (addingOrigin) {
                                originForm.reset();
                                setAddingOrigin(false);
                                return;
                            }
                            originForm.reset({
                                group: website.config.default_origin_group,
                                protocol: 'http',
                                host: '',
                                port: 80,
                                role: 'primary',
                                weight: 10,
                                status: 'enabled',
                            });
                            setAddingOrigin(true);
                        }}
                    >
                        {addingOrigin ? (
                            '取消添加'
                        ) : (
                            <>
                                <Plus />
                                添加源站
                            </>
                        )}
                    </Button>
                }
            >
                {addingOrigin && (
                    <form
                        noValidate
                        className="mb-5 grid gap-4 rounded-lg border bg-muted/20 p-4 sm:grid-cols-2 lg:grid-cols-4"
                        onSubmit={originForm.handleSubmit((data) =>
                            save.mutate(
                                {
                                    target: websiteSaveTarget(website),
                                    input: {
                                        ...currentWebsiteInput(website),
                                        config: {
                                            ...website.config,
                                            origins: [
                                                ...website.config.origins,
                                                {
                                                    id: createUuid(),
                                                    ...data,
                                                    host: data.host.toLowerCase(),
                                                },
                                            ],
                                        },
                                    },
                                },
                                {
                                    onSuccess: () => {
                                        originForm.reset();
                                        setAddingOrigin(false);
                                    },
                                }
                            )
                        )}
                    >
                        <FormControl
                            id="origin-group"
                            label="源站组"
                            error={originForm.formState.errors.group?.message}
                        >
                            <Input
                                id="origin-group"
                                maxLength={100}
                                placeholder="例如 default、api"
                                aria-invalid={Boolean(originForm.formState.errors.group)}
                                {...originForm.register('group')}
                            />
                        </FormControl>
                        <Controller
                            control={originForm.control}
                            name="protocol"
                            render={({ field, fieldState }) => (
                                <FormControl
                                    id="origin-protocol"
                                    label="协议"
                                    error={fieldState.error?.message}
                                >
                                    <Select
                                        value={field.value}
                                        onValueChange={(value: 'http' | 'https') => {
                                            field.onChange(value);
                                            originForm.setValue(
                                                'port',
                                                value === 'https' ? 443 : 80,
                                                {
                                                    shouldDirty: true,
                                                }
                                            );
                                        }}
                                    >
                                        <SelectTrigger id="origin-protocol" className="w-full">
                                            <SelectValue />
                                        </SelectTrigger>
                                        <SelectContent>
                                            <SelectItem value="http">HTTP</SelectItem>
                                            <SelectItem value="https">HTTPS</SelectItem>
                                        </SelectContent>
                                    </Select>
                                </FormControl>
                            )}
                        />
                        <FormControl
                            id="origin-host"
                            label="源站地址"
                            error={originForm.formState.errors.host?.message}
                            className="sm:col-span-2"
                        >
                            <Input
                                id="origin-host"
                                placeholder="10.0.0.1 或 origin.example.com"
                                aria-invalid={Boolean(originForm.formState.errors.host)}
                                {...originForm.register('host')}
                            />
                        </FormControl>
                        <FormControl
                            id="origin-port"
                            label="端口"
                            error={originForm.formState.errors.port?.message}
                        >
                            <Input
                                id="origin-port"
                                type="number"
                                min={1}
                                max={65535}
                                aria-invalid={Boolean(originForm.formState.errors.port)}
                                {...originForm.register('port', { valueAsNumber: true })}
                            />
                        </FormControl>
                        <Controller
                            control={originForm.control}
                            name="role"
                            render={({ field, fieldState }) => (
                                <FormControl
                                    id="origin-role"
                                    label="角色"
                                    error={fieldState.error?.message}
                                >
                                    <Select value={field.value} onValueChange={field.onChange}>
                                        <SelectTrigger id="origin-role" className="w-full">
                                            <SelectValue />
                                        </SelectTrigger>
                                        <SelectContent>
                                            <SelectItem value="primary">主源站</SelectItem>
                                            <SelectItem value="backup">备用源站</SelectItem>
                                        </SelectContent>
                                    </Select>
                                </FormControl>
                            )}
                        />
                        <FormControl
                            id="origin-weight"
                            label="权重"
                            error={originForm.formState.errors.weight?.message}
                        >
                            <Input
                                id="origin-weight"
                                type="number"
                                min={1}
                                max={100}
                                aria-invalid={Boolean(originForm.formState.errors.weight)}
                                {...originForm.register('weight', { valueAsNumber: true })}
                            />
                        </FormControl>
                        <Controller
                            control={originForm.control}
                            name="status"
                            render={({ field, fieldState }) => (
                                <FormControl
                                    id="origin-status"
                                    label="状态"
                                    error={fieldState.error?.message}
                                >
                                    <Select value={field.value} onValueChange={field.onChange}>
                                        <SelectTrigger id="origin-status" className="w-full">
                                            <SelectValue />
                                        </SelectTrigger>
                                        <SelectContent>
                                            <SelectItem value="enabled">启用</SelectItem>
                                            <SelectItem value="disabled">停用</SelectItem>
                                        </SelectContent>
                                    </Select>
                                </FormControl>
                            )}
                        />
                        <div className="flex items-end">
                            <Button type="submit" className="w-full" disabled={save.isPending}>
                                {save.isPending ? (
                                    <LoaderCircle className="animate-spin" />
                                ) : (
                                    <Plus />
                                )}
                                添加源站
                            </Button>
                        </div>
                    </form>
                )}
                <DataTable
                    columns={originColumns}
                    data={websiteOrigins}
                    getRowKey={(item) => item.id}
                    emptyTitle="暂无源站"
                />
            </SectionCard>
            <SectionCard title="回源设置" description="应用于当前网站的全部源站">
                <form
                    noValidate
                    className="space-y-4"
                    onSubmit={originSettingsForm.handleSubmit((data) =>
                        save.mutate(
                            {
                                target: websiteSaveTarget(website),
                                input: {
                                    ...currentWebsiteInput(website),
                                    config: {
                                        ...website.config,
                                        ...data,
                                        origin_host_header:
                                            data.origin_host_header.trim() || '$host',
                                    },
                                },
                            },
                            { onSuccess: () => originSettingsForm.reset(data) }
                        )
                    )}
                >
                    <FormControl
                        id="default-origin-group"
                        label="默认源站组"
                        error={originSettingsForm.formState.errors.default_origin_group?.message}
                        description="未命中路由规则时使用；该组至少需要一个启用的主源站"
                    >
                        <Input
                            id="default-origin-group"
                            maxLength={100}
                            placeholder="default"
                            aria-invalid={Boolean(
                                originSettingsForm.formState.errors.default_origin_group
                            )}
                            {...originSettingsForm.register('default_origin_group')}
                        />
                    </FormControl>
                    <FormControl
                        id="origin-host-header"
                        label="回源 Host"
                        error={originSettingsForm.formState.errors.origin_host_header?.message}
                        description="留空时使用访问域名"
                    >
                        <Input
                            id="origin-host-header"
                            placeholder="留空则使用访问域名"
                            aria-invalid={Boolean(
                                originSettingsForm.formState.errors.origin_host_header
                            )}
                            {...originSettingsForm.register('origin_host_header')}
                        />
                    </FormControl>
                    <div className="grid gap-4 sm:grid-cols-2">
                        <FormControl
                            id="origin-connect-timeout"
                            label="连接超时（秒）"
                            error={
                                originSettingsForm.formState.errors.origin_connect_timeout_seconds
                                    ?.message
                            }
                        >
                            <Input
                                id="origin-connect-timeout"
                                type="number"
                                min={1}
                                max={300}
                                aria-invalid={Boolean(
                                    originSettingsForm.formState.errors
                                        .origin_connect_timeout_seconds
                                )}
                                {...originSettingsForm.register('origin_connect_timeout_seconds', {
                                    valueAsNumber: true,
                                })}
                            />
                        </FormControl>
                        <FormControl
                            id="origin-read-timeout"
                            label="读取超时（秒）"
                            error={
                                originSettingsForm.formState.errors.origin_read_timeout_seconds
                                    ?.message
                            }
                        >
                            <Input
                                id="origin-read-timeout"
                                type="number"
                                min={1}
                                max={600}
                                aria-invalid={Boolean(
                                    originSettingsForm.formState.errors.origin_read_timeout_seconds
                                )}
                                {...originSettingsForm.register('origin_read_timeout_seconds', {
                                    valueAsNumber: true,
                                })}
                            />
                        </FormControl>
                    </div>
                    <div className="grid gap-3 sm:grid-cols-2">
                        <Controller
                            control={originSettingsForm.control}
                            name="pass_client_ip"
                            render={({ field }) => (
                                <SwitchRow
                                    id="pass-client-ip"
                                    label="传递真实访客 IP"
                                    checked={field.value}
                                    onCheckedChange={field.onChange}
                                />
                            )}
                        />
                        <Controller
                            control={originSettingsForm.control}
                            name="health_check_enabled"
                            render={({ field }) => (
                                <SwitchRow
                                    id="health-check"
                                    label="源站健康检查"
                                    checked={field.value}
                                    onCheckedChange={field.onChange}
                                />
                            )}
                        />
                    </div>
                    <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-3">
                        <FormControl
                            id="health-check-path"
                            label="健康检查路径"
                            error={originSettingsForm.formState.errors.health_check_path?.message}
                        >
                            <Input
                                id="health-check-path"
                                placeholder="/health"
                                disabled={!originSettingsForm.watch('health_check_enabled')}
                                aria-invalid={Boolean(
                                    originSettingsForm.formState.errors.health_check_path
                                )}
                                {...originSettingsForm.register('health_check_path')}
                            />
                        </FormControl>
                        {(
                            [
                                ['health_check_interval_seconds', '检查间隔（秒）', 1, 3600],
                                ['health_check_timeout_seconds', '检查超时（秒）', 1, 300],
                                ['health_check_expected_status', '期望状态码', 100, 599],
                                ['healthy_threshold', '健康恢复阈值', 1, 10],
                                ['unhealthy_threshold', '故障判定阈值', 1, 10],
                            ] as const
                        ).map(([name, label, min, max]) => (
                            <FormControl
                                key={name}
                                id={name.replaceAll('_', '-')}
                                label={label}
                                error={originSettingsForm.formState.errors[name]?.message}
                            >
                                <Input
                                    id={name.replaceAll('_', '-')}
                                    type="number"
                                    min={min}
                                    max={max}
                                    disabled={!originSettingsForm.watch('health_check_enabled')}
                                    aria-invalid={Boolean(
                                        originSettingsForm.formState.errors[name]
                                    )}
                                    {...originSettingsForm.register(name, { valueAsNumber: true })}
                                />
                            </FormControl>
                        ))}
                    </div>
                    <div className="flex justify-end">
                        <SubmitButton pending={save.isPending}>保存回源设置</SubmitButton>
                    </div>
                </form>
            </SectionCard>
        </div>
    );

    const routesSection = website && (
        <div className="space-y-4">
            <SectionCard
                title="路由规则"
                description="按顺序匹配请求路径；未命中时使用默认源站组"
                action={
                    <Button
                        variant={addingRoute ? 'outline' : 'default'}
                        size="sm"
                        onClick={() => {
                            if (addingRoute) {
                                routeForm.reset();
                                setAddingRoute(false);
                                return;
                            }
                            routeForm.reset({
                                status: 'enabled',
                                match_type: 'prefix',
                                path: '/',
                                methods: [],
                                action: 'proxy',
                                rewrite_path: '',
                                redirect_url: '',
                                redirect_status: 302,
                                origin_group: website.config.default_origin_group,
                                request_headers_text: '',
                                response_headers_text: '',
                            });
                            setAddingRoute(true);
                        }}
                    >
                        {addingRoute ? (
                            '取消添加'
                        ) : (
                            <>
                                <Plus />
                                添加规则
                            </>
                        )}
                    </Button>
                }
            >
                {addingRoute && (
                    <form
                        noValidate
                        className="mb-5 space-y-4 rounded-lg border bg-muted/20 p-4"
                        onSubmit={routeForm.handleSubmit((data) =>
                            save.mutate(
                                {
                                    target: websiteSaveTarget(website),
                                    input: {
                                        ...currentWebsiteInput(website),
                                        config: {
                                            ...website.config,
                                            route_rules: [
                                                ...website.config.route_rules,
                                                {
                                                    id: createUuid(),
                                                    status: data.status,
                                                    match_type: data.match_type,
                                                    path: data.path,
                                                    methods: data.methods,
                                                    action: data.action,
                                                    rewrite_path:
                                                        data.action === 'proxy'
                                                            ? data.rewrite_path
                                                            : '',
                                                    redirect_url:
                                                        data.action === 'redirect'
                                                            ? data.redirect_url
                                                            : '',
                                                    redirect_status:
                                                        data.action === 'redirect'
                                                            ? data.redirect_status
                                                            : 0,
                                                    origin_group:
                                                        data.action === 'proxy'
                                                            ? data.origin_group
                                                            : '',
                                                    request_headers: parseRouteHeaders(
                                                        data.request_headers_text
                                                    ),
                                                    response_headers: parseRouteHeaders(
                                                        data.response_headers_text
                                                    ),
                                                },
                                            ],
                                        },
                                    },
                                },
                                {
                                    onSuccess: () => {
                                        routeForm.reset();
                                        setAddingRoute(false);
                                    },
                                }
                            )
                        )}
                    >
                        <div className="grid gap-4 sm:grid-cols-2 xl:grid-cols-4">
                            <FormControl
                                id="route-path"
                                label="匹配路径"
                                error={routeForm.formState.errors.path?.message}
                            >
                                <Input
                                    id="route-path"
                                    placeholder="/api/"
                                    aria-invalid={Boolean(routeForm.formState.errors.path)}
                                    {...routeForm.register('path')}
                                />
                            </FormControl>
                            <Controller
                                control={routeForm.control}
                                name="match_type"
                                render={({ field }) => (
                                    <FormControl id="route-match-type" label="匹配方式">
                                        <Select value={field.value} onValueChange={field.onChange}>
                                            <SelectTrigger id="route-match-type" className="w-full">
                                                <SelectValue />
                                            </SelectTrigger>
                                            <SelectContent>
                                                <SelectItem value="prefix">前缀匹配</SelectItem>
                                                <SelectItem value="exact">精确匹配</SelectItem>
                                            </SelectContent>
                                        </Select>
                                    </FormControl>
                                )}
                            />
                            <Controller
                                control={routeForm.control}
                                name="action"
                                render={({ field }) => (
                                    <FormControl id="route-action" label="动作">
                                        <Select value={field.value} onValueChange={field.onChange}>
                                            <SelectTrigger id="route-action" className="w-full">
                                                <SelectValue />
                                            </SelectTrigger>
                                            <SelectContent>
                                                <SelectItem value="proxy">代理到源站</SelectItem>
                                                <SelectItem value="redirect">HTTP 跳转</SelectItem>
                                            </SelectContent>
                                        </Select>
                                    </FormControl>
                                )}
                            />
                            <Controller
                                control={routeForm.control}
                                name="status"
                                render={({ field }) => (
                                    <FormControl id="route-status" label="状态">
                                        <Select value={field.value} onValueChange={field.onChange}>
                                            <SelectTrigger id="route-status" className="w-full">
                                                <SelectValue />
                                            </SelectTrigger>
                                            <SelectContent>
                                                <SelectItem value="enabled">启用</SelectItem>
                                                <SelectItem value="disabled">停用</SelectItem>
                                            </SelectContent>
                                        </Select>
                                    </FormControl>
                                )}
                            />
                            {routeAction === 'proxy' ? (
                                <>
                                    <FormControl
                                        id="route-origin-group"
                                        label="源站组"
                                        error={routeForm.formState.errors.origin_group?.message}
                                    >
                                        <Input
                                            id="route-origin-group"
                                            placeholder={website.config.default_origin_group}
                                            aria-invalid={Boolean(
                                                routeForm.formState.errors.origin_group
                                            )}
                                            {...routeForm.register('origin_group')}
                                        />
                                    </FormControl>
                                    <FormControl
                                        id="route-rewrite-path"
                                        label="重写路径（可选）"
                                        error={routeForm.formState.errors.rewrite_path?.message}
                                    >
                                        <Input
                                            id="route-rewrite-path"
                                            placeholder="/upstream/"
                                            aria-invalid={Boolean(
                                                routeForm.formState.errors.rewrite_path
                                            )}
                                            {...routeForm.register('rewrite_path')}
                                        />
                                    </FormControl>
                                </>
                            ) : (
                                <>
                                    <FormControl
                                        id="route-redirect-url"
                                        label="跳转地址"
                                        error={routeForm.formState.errors.redirect_url?.message}
                                        className="sm:col-span-2"
                                    >
                                        <Input
                                            id="route-redirect-url"
                                            placeholder="https://example.com/new-path"
                                            aria-invalid={Boolean(
                                                routeForm.formState.errors.redirect_url
                                            )}
                                            {...routeForm.register('redirect_url')}
                                        />
                                    </FormControl>
                                    <Controller
                                        control={routeForm.control}
                                        name="redirect_status"
                                        render={({ field }) => (
                                            <FormControl
                                                id="route-redirect-status"
                                                label="跳转状态码"
                                            >
                                                <Select
                                                    value={String(field.value)}
                                                    onValueChange={(value) =>
                                                        field.onChange(Number(value) as 301 | 302)
                                                    }
                                                >
                                                    <SelectTrigger
                                                        id="route-redirect-status"
                                                        className="w-full"
                                                    >
                                                        <SelectValue />
                                                    </SelectTrigger>
                                                    <SelectContent>
                                                        <SelectItem value="302">
                                                            302 临时跳转
                                                        </SelectItem>
                                                        <SelectItem value="301">
                                                            301 永久跳转
                                                        </SelectItem>
                                                    </SelectContent>
                                                </Select>
                                            </FormControl>
                                        )}
                                    />
                                </>
                            )}
                        </div>
                        <Controller
                            control={routeForm.control}
                            name="methods"
                            render={({ field, fieldState }) => (
                                <FormControl
                                    id="route-methods"
                                    label="请求方法"
                                    error={fieldState.error?.message}
                                    description="不选择表示匹配全部方法"
                                >
                                    <div className="flex flex-wrap gap-3 rounded-lg border bg-background p-3">
                                        {routeMethodOptions.map((method) => (
                                            <label
                                                key={method}
                                                htmlFor={`route-method-${method.toLowerCase()}`}
                                                className="flex cursor-pointer items-center gap-2 text-sm"
                                            >
                                                <Checkbox
                                                    id={`route-method-${method.toLowerCase()}`}
                                                    checked={field.value.includes(method)}
                                                    onCheckedChange={(checked) =>
                                                        field.onChange(
                                                            checked === true
                                                                ? [...field.value, method]
                                                                : field.value.filter(
                                                                      (value) => value !== method
                                                                  )
                                                        )
                                                    }
                                                />
                                                {method}
                                            </label>
                                        ))}
                                    </div>
                                </FormControl>
                            )}
                        />
                        <div className="grid gap-4 lg:grid-cols-2">
                            <FormControl
                                id="route-request-headers"
                                label="请求 Header"
                                error={routeForm.formState.errors.request_headers_text?.message}
                                description="每行一项，格式 Header-Name: value"
                            >
                                <Textarea
                                    id="route-request-headers"
                                    placeholder="X-Edge-Route: api"
                                    {...routeForm.register('request_headers_text')}
                                />
                            </FormControl>
                            <FormControl
                                id="route-response-headers"
                                label="响应 Header"
                                error={routeForm.formState.errors.response_headers_text?.message}
                                description="每行一项，格式 Header-Name: value"
                            >
                                <Textarea
                                    id="route-response-headers"
                                    placeholder="Cache-Control: public, max-age=60"
                                    {...routeForm.register('response_headers_text')}
                                />
                            </FormControl>
                        </div>
                        <div className="flex justify-end">
                            <SubmitButton pending={save.isPending}>添加路由规则</SubmitButton>
                        </div>
                    </form>
                )}
                <DataTable
                    columns={routeColumns}
                    data={website.config.route_rules}
                    getRowKey={(item) => item.id}
                    emptyTitle="暂无路由规则"
                />
            </SectionCard>
        </div>
    );

    const compressionSection = website && (
        <SectionCard title="内容压缩" description="按响应类型和大小选择压缩算法">
            <form
                noValidate
                className="space-y-5"
                onSubmit={compressionForm.handleSubmit((data) =>
                    save.mutate(
                        {
                            target: websiteSaveTarget(website),
                            input: {
                                ...currentWebsiteInput(website),
                                config: { ...website.config, ...data },
                            },
                        },
                        { onSuccess: () => compressionForm.reset(data) }
                    )
                )}
            >
                <Controller
                    control={compressionForm.control}
                    name="response_compression_enabled"
                    render={({ field }) => (
                        <SwitchRow
                            id="compression-enabled"
                            label="启用内容压缩"
                            description="仅在客户端支持且压缩后体积更小时生效"
                            checked={field.value}
                            onCheckedChange={field.onChange}
                        />
                    )}
                />
                <div className="grid gap-4 sm:grid-cols-2">
                    <FormControl
                        id="compression-min-bytes"
                        label="最小压缩大小（字节）"
                        error={
                            compressionForm.formState.errors.response_compression_min_bytes?.message
                        }
                    >
                        <Input
                            id="compression-min-bytes"
                            type="number"
                            min={256}
                            max={1048576}
                            aria-invalid={Boolean(
                                compressionForm.formState.errors.response_compression_min_bytes
                            )}
                            {...compressionForm.register('response_compression_min_bytes', {
                                valueAsNumber: true,
                            })}
                        />
                    </FormControl>
                    <FormControl
                        id="compression-max-bytes"
                        label="最大压缩大小（字节）"
                        error={
                            compressionForm.formState.errors.response_compression_max_bytes?.message
                        }
                        description="0 表示不限制，数据面仍保留 64 MiB 安全上限"
                    >
                        <Input
                            id="compression-max-bytes"
                            type="number"
                            min={0}
                            max={67108864}
                            aria-invalid={Boolean(
                                compressionForm.formState.errors.response_compression_max_bytes
                            )}
                            {...compressionForm.register('response_compression_max_bytes', {
                                valueAsNumber: true,
                            })}
                        />
                    </FormControl>
                </div>
                <Controller
                    control={compressionForm.control}
                    name="response_compression_algorithms"
                    render={({ field, fieldState }) => (
                        <FormControl
                            id="compression-algorithms"
                            label="压缩算法（按优先级）"
                            error={fieldState.error?.message}
                            description="客户端质量值相同时按这里的顺序选择"
                        >
                            <AlgorithmOrder
                                value={field.value}
                                onChange={field.onChange}
                                invalid={Boolean(fieldState.error)}
                            />
                        </FormControl>
                    )}
                />
                <Controller
                    control={compressionForm.control}
                    name="response_compression_mime_types"
                    render={({ field, fieldState }) => (
                        <FormControl
                            id="compression-mime-types"
                            label="支持的 MIME 类型"
                            error={fieldState.error?.message}
                        >
                            <TagEditor
                                value={field.value}
                                onChange={field.onChange}
                                suggestions={responseCompressionMimeTypeOptions}
                                placeholder="例如 application/json"
                                invalid={Boolean(fieldState.error)}
                            />
                        </FormControl>
                    )}
                />
                <div className="grid gap-4 lg:grid-cols-2">
                    <Controller
                        control={compressionForm.control}
                        name="response_compression_extensions"
                        render={({ field, fieldState }) => (
                            <FormControl
                                id="compression-extensions"
                                label="支持的扩展名"
                                error={fieldState.error?.message}
                            >
                                <TagEditor
                                    value={field.value}
                                    onChange={field.onChange}
                                    placeholder="例如 .js"
                                    invalid={Boolean(fieldState.error)}
                                />
                            </FormControl>
                        )}
                    />
                    <Controller
                        control={compressionForm.control}
                        name="response_compression_excluded_extensions"
                        render={({ field, fieldState }) => (
                            <FormControl
                                id="compression-excluded-extensions"
                                label="例外扩展名"
                                error={fieldState.error?.message}
                                description="优先于 MIME 类型和支持扩展名"
                            >
                                <TagEditor
                                    value={field.value}
                                    onChange={field.onChange}
                                    placeholder="例如 .apk"
                                    invalid={Boolean(fieldState.error)}
                                />
                            </FormControl>
                        )}
                    />
                </div>
                <div className="flex justify-end">
                    <SubmitButton pending={save.isPending}>保存内容压缩设置</SubmitButton>
                </div>
            </form>
        </SectionCard>
    );

    const httpsSection = website && (
        <SectionCard title="HTTPS 加速" description="证书按绑定域名自动匹配">
            <form
                noValidate
                className="space-y-5"
                onSubmit={httpsForm.handleSubmit((data) =>
                    save.mutate(
                        {
                            target: websiteSaveTarget(website),
                            input: {
                                ...currentWebsiteInput(website),
                                config: {
                                    ...website.config,
                                    ...data,
                                    certificate_ids: data.https_enabled ? data.certificate_ids : [],
                                },
                            },
                        },
                        {
                            onSuccess: () =>
                                httpsForm.reset({
                                    ...data,
                                    certificate_ids: data.https_enabled ? data.certificate_ids : [],
                                }),
                        }
                    )
                )}
            >
                <Controller
                    control={httpsForm.control}
                    name="https_enabled"
                    render={({ field }) => (
                        <SwitchRow
                            id="https-enabled"
                            label="为证书覆盖的域名启用 HTTPS"
                            checked={field.value}
                            onCheckedChange={field.onChange}
                        />
                    )}
                />
                {!httpsEnabled && (
                    <Alert>
                        <ShieldCheck />
                        <AlertTitle>HTTPS 未开启</AlertTitle>
                        <AlertDescription>
                            当前所有绑定域名仅提供 HTTP；开启并选择证书后，系统会按域名自动匹配。
                        </AlertDescription>
                    </Alert>
                )}
                <Controller
                    control={httpsForm.control}
                    name="certificate_ids"
                    render={({ field, fieldState }) => (
                        <FormControl
                            id="https-certificates"
                            label="绑定证书（可多选）"
                            error={fieldState.error?.message}
                        >
                            <CertificatePicker
                                value={field.value}
                                options={certificateOptions}
                                onChange={field.onChange}
                                onSearchChange={setCertificateKeyword}
                                loading={certificates.isLoading || certificates.isFetching}
                                disabled={!httpsEnabled}
                                invalid={Boolean(fieldState.error)}
                            />
                        </FormControl>
                    )}
                />
                {httpsEnabled && uncoveredDomains.length > 0 && (
                    <Alert variant="destructive">
                        <ShieldCheck />
                        <AlertTitle>部分域名将保持 HTTP</AlertTitle>
                        <AlertDescription>
                            以下域名未被所选证书覆盖，不会开启 HTTPS：
                            {uncoveredDomains.join('、')}
                        </AlertDescription>
                    </Alert>
                )}
                <div className="grid gap-4 sm:grid-cols-2">
                    <Controller
                        control={httpsForm.control}
                        name="minimum_tls_version"
                        render={({ field }) => (
                            <FormControl id="minimum-tls-version" label="最低 TLS 版本">
                                <Select
                                    value={field.value}
                                    onValueChange={field.onChange}
                                    disabled={!httpsEnabled}
                                >
                                    <SelectTrigger id="minimum-tls-version" className="w-full">
                                        <SelectValue />
                                    </SelectTrigger>
                                    <SelectContent>
                                        <SelectItem value="1.2">TLS 1.2（推荐）</SelectItem>
                                        <SelectItem value="1.3">TLS 1.3</SelectItem>
                                    </SelectContent>
                                </Select>
                            </FormControl>
                        )}
                    />
                    <Controller
                        control={httpsForm.control}
                        name="force_https"
                        render={({ field }) => (
                            <SwitchRow
                                id="force-https"
                                label="自动跳转 HTTPS"
                                checked={field.value}
                                onCheckedChange={field.onChange}
                                disabled={!httpsEnabled}
                            />
                        )}
                    />
                    <Controller
                        control={httpsForm.control}
                        name="http2_enabled"
                        render={({ field }) => (
                            <SwitchRow
                                id="http2-enabled"
                                label="HTTP/2"
                                checked={field.value}
                                onCheckedChange={field.onChange}
                                disabled={!httpsEnabled}
                            />
                        )}
                    />
                    <Controller
                        control={httpsForm.control}
                        name="hsts_enabled"
                        render={({ field }) => (
                            <SwitchRow
                                id="hsts-enabled"
                                label="HSTS"
                                checked={field.value}
                                onCheckedChange={field.onChange}
                                disabled={!httpsEnabled}
                            />
                        )}
                    />
                </div>
                <div className="flex justify-end">
                    <SubmitButton pending={save.isPending}>保存 HTTPS 设置</SubmitButton>
                </div>
            </form>
        </SectionCard>
    );

    const accessLogSection = website && (
        <SectionCard title="访问日志" description="控制节点上报和存储的请求字段">
            <form
                noValidate
                className="space-y-5"
                onSubmit={accessLogForm.handleSubmit((data) =>
                    save.mutate(
                        {
                            target: websiteSaveTarget(website),
                            input: {
                                ...currentWebsiteInput(website),
                                config: { ...website.config, ...data },
                            },
                        },
                        { onSuccess: () => accessLogForm.reset(data) }
                    )
                )}
            >
                <Controller
                    control={accessLogForm.control}
                    name="access_log_enabled"
                    render={({ field }) => (
                        <SwitchRow
                            id="access-log-enabled"
                            label="启用访问日志"
                            checked={field.value}
                            onCheckedChange={field.onChange}
                        />
                    )}
                />
                <Alert>
                    <FileText />
                    <AlertTitle>基础信息始终一致</AlertTitle>
                    <AlertDescription>
                        默认记录客户端 IP、请求 URL、协议、状态码、响应大小、耗时和节点信息。
                    </AlertDescription>
                </Alert>
                <div className="grid gap-3 sm:grid-cols-2 xl:grid-cols-3">
                    {(
                        [
                            ['access_log_request_headers', '请求 Header 列表'],
                            ['access_log_response_headers', '响应 Header 列表'],
                            ['access_log_query_params', '参数列表'],
                            ['access_log_cookies', 'Cookie 列表'],
                            ['access_log_referer', '请求来源'],
                            ['access_log_user_agent', '终端信息'],
                            ['access_log_request_body', '请求 Body'],
                        ] as const
                    ).map(([name, label]) => (
                        <Controller
                            key={name}
                            control={accessLogForm.control}
                            name={name}
                            render={({ field }) => (
                                <SwitchRow
                                    id={name.replaceAll('_', '-')}
                                    label={label}
                                    checked={field.value}
                                    onCheckedChange={field.onChange}
                                    disabled={!accessLogEnabled}
                                />
                            )}
                        />
                    ))}
                    <SwitchRow
                        id="location-analysis"
                        label="位置和浏览器分析"
                        checked={false}
                        onCheckedChange={() => undefined}
                        disabled
                    />
                </div>
                <Alert variant="destructive">
                    <Gauge />
                    <AlertTitle>请求 Body 可能产生大量数据</AlertTitle>
                    <AlertDescription>
                        建议仅调试时启用，单次请求最大记录尺寸为 2 MiB。
                    </AlertDescription>
                </Alert>
                <Controller
                    control={accessLogForm.control}
                    name="access_log_status_code_ranges"
                    render={({ field, fieldState }) => (
                        <FormControl
                            id="access-log-status-ranges"
                            label="要存储的访问日志状态码"
                            error={fieldState.error?.message}
                        >
                            <div className="flex flex-wrap gap-3 rounded-lg border p-3">
                                {accessLogStatusCodeRangeOptions.map((option) => {
                                    const checked = field.value.includes(option.value);
                                    return (
                                        <label
                                            key={option.value}
                                            htmlFor={`access-log-status-${option.value}`}
                                            className="flex cursor-pointer items-center gap-2 text-sm"
                                        >
                                            <Checkbox
                                                id={`access-log-status-${option.value}`}
                                                checked={checked}
                                                disabled={!accessLogEnabled}
                                                onCheckedChange={(nextChecked) =>
                                                    field.onChange(
                                                        nextChecked === true
                                                            ? [...field.value, option.value]
                                                            : field.value.filter(
                                                                  (value) => value !== option.value
                                                              )
                                                    )
                                                }
                                            />
                                            {option.label}
                                        </label>
                                    );
                                })}
                            </div>
                        </FormControl>
                    )}
                />
                <Controller
                    control={accessLogForm.control}
                    name="access_log_client_abort"
                    render={({ field }) => (
                        <SwitchRow
                            id="access-log-client-abort"
                            label="记录客户端中断日志"
                            description="以 499 状态码记录客户端主动中断日志"
                            checked={field.value}
                            onCheckedChange={field.onChange}
                            disabled={!accessLogEnabled}
                        />
                    )}
                />
                <div className="flex justify-end">
                    <SubmitButton pending={save.isPending}>保存访问日志设置</SubmitButton>
                </div>
            </form>
        </SectionCard>
    );

    return (
        <>
            <Sheet open={websiteId !== undefined} onOpenChange={(open) => !open && close()}>
                <SheetContent
                    className="w-full gap-0 p-0 sm:max-w-[min(1120px,94vw)]"
                    onEscapeKeyDown={(event) => save.isPending && event.preventDefault()}
                    onPointerDownOutside={(event) => save.isPending && event.preventDefault()}
                >
                    <SheetHeader className="border-b px-5 py-4 pr-12">
                        <div className="flex min-w-0 flex-wrap items-center gap-2">
                            <SheetTitle className="truncate">
                                {website ? `网站配置 · ${website.website_name}` : '网站配置'}
                            </SheetTitle>
                            {website && (
                                <StatusBadge tone={websiteStatusMeta[website.status].tone}>
                                    {websiteStatusMeta[website.status].label}
                                </StatusBadge>
                            )}
                            {hasUnsavedChanges && <Badge variant="warning">有未保存修改</Badge>}
                        </div>
                        <SheetDescription>
                            各分区独立保存，并使用当前 revision 防止覆盖并发修改
                        </SheetDescription>
                    </SheetHeader>
                    {detail.isLoading ? (
                        <DetailLoading />
                    ) : website ? (
                        <Tabs
                            value={activeTab}
                            onValueChange={(value) => isDetailTab(value) && setActiveTab(value)}
                            className="min-h-0 flex-1 gap-0 overflow-hidden"
                        >
                            <div className="shrink-0 overflow-x-auto border-b bg-muted/15 px-3">
                                <TabsList variant="line" className="h-12 min-w-max gap-1">
                                    {detailTabs.map((tab) => (
                                        <TabsTrigger key={tab.value} value={tab.value}>
                                            {tab.icon}
                                            {tab.label}
                                            {dirtySections[tab.value] && (
                                                <span
                                                    className="size-1.5 rounded-full bg-amber-500"
                                                    title="有未保存修改"
                                                />
                                            )}
                                        </TabsTrigger>
                                    ))}
                                </TabsList>
                            </div>
                            <TabsContent
                                value="basic"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {basicSection}
                            </TabsContent>
                            <TabsContent
                                value="domains"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {domainsSection}
                            </TabsContent>
                            <TabsContent
                                value="origins"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {originsSection}
                            </TabsContent>
                            <TabsContent
                                value="routes"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {routesSection}
                            </TabsContent>
                            <TabsContent
                                value="compression"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {compressionSection}
                            </TabsContent>
                            <TabsContent
                                value="https"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {httpsSection}
                            </TabsContent>
                            <TabsContent
                                value="accessLog"
                                className="min-h-0 overflow-y-auto p-4 sm:p-5"
                            >
                                {accessLogSection}
                            </TabsContent>
                        </Tabs>
                    ) : (
                        <EmptyState title="网站不存在或已删除" className="flex-1" />
                    )}
                </SheetContent>
            </Sheet>
            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </>
    );
}

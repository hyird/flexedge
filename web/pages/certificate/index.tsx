import { zodResolver } from '@hookform/resolvers/zod';
import {
    ArrowLeft,
    ArrowRight,
    Check,
    CircleAlert,
    Clipboard,
    Download,
    MoreHorizontal,
    Plus,
    RefreshCw,
    RotateCcw,
    Search,
    Settings2,
    Trash2,
} from 'lucide-react';
import { useEffect, useState } from 'react';
import { Controller, useForm } from 'react-hook-form';
import { toast } from '@/components/ui/notification';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { DescriptionList } from '@/components/description_list';
import { EmptyState } from '@/components/empty_state';
import { PageHeader } from '@/components/page_header';
import { PaginationBar } from '@/components/pagination_bar';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import {
    DropdownMenu,
    DropdownMenuContent,
    DropdownMenuItem,
    DropdownMenuSeparator,
    DropdownMenuTrigger,
} from '@/components/ui/dropdown_menu';
import { Field, FieldDescription, FieldError, FieldGroup, FieldLabel } from '@/components/ui/field';
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
    SheetFooter,
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { Spinner } from '@/components/ui/spinner';
import { Switch } from '@/components/ui/switch';
import { Tooltip, TooltipContent, TooltipProvider, TooltipTrigger } from '@/components/ui/tooltip';
import { CERTIFICATE_PROVIDER_REGISTRY } from '@/config/providers';
import DnsZoneSelect from '@/pages/dns_zone/components/dns_zone_select';
import { useDnsZoneOptions } from '@/pages/dns_zone/dns_zone.service';
import { useCertificateProviderList } from '@/pages/provider/certificate_provider.service';
import type { CertificateProviderType } from '@/pages/provider/certificate_provider.types';
import CertificateProviderManager from '@/pages/provider/certificate_provider_manager';
import DnsProviderTag from '@/pages/provider/components/dns_provider_tag';
import { formatDateTime } from '@/utils/date';
import {
    type CertificateCreateValues,
    certificateCreateSchema,
    DOMAIN_MAX_LENGTH,
} from './certificate.schema';
import {
    useCertificateCreate,
    useCertificateDelete,
    useCertificateDetail,
    useCertificateDownload,
    useCertificateList,
    useCertificateRenew,
    useCertificateSave,
} from './certificate.service';
import type {
    CertificateItem,
    CertificateQuery,
    CertificateStatus,
    CertificateSyncStatus,
} from './certificate.types';

const certificateStatusMeta: Record<
    CertificateStatus,
    {
        label: string;
        tone: 'success' | 'warning' | 'destructive' | 'info' | 'neutral';
    }
> = {
    pending: { label: '待提交', tone: 'neutral' },
    issuing: { label: '正在验证', tone: 'info' },
    valid: { label: '已签发', tone: 'success' },
    renewing: { label: '续签中', tone: 'info' },
    failed: { label: '失败', tone: 'destructive' },
    expired: { label: '已过期', tone: 'warning' },
};

const syncStatusMeta: Record<CertificateSyncStatus, string> = {
    pending: '待同步',
    running: '同步中',
    retry: '等待重试',
    completed: '已完成',
};

const providerTypeMeta = Object.fromEntries(
    Object.entries(CERTIFICATE_PROVIDER_REGISTRY).map(([code, provider]) => [code, provider.label])
) as Record<CertificateProviderType, string>;

const createDefaults: CertificateCreateValues = {
    domain: '',
    certificate_provider_id: '',
    dns_zone_id: '',
    auto_renew: true,
};

function normalizedCertificateDomain(value?: string) {
    return value?.trim().toLowerCase().replace(/\.$/, '') ?? '';
}

async function copyText(text: string) {
    if (window.isSecureContext && navigator.clipboard?.writeText) {
        try {
            await navigator.clipboard.writeText(text);
            return;
        } catch {
            // Fall through to the synchronous browser fallback.
        }
    }

    const textarea = document.createElement('textarea');
    const activeElement =
        document.activeElement instanceof HTMLElement ? document.activeElement : null;
    textarea.value = text;
    textarea.setAttribute('readonly', '');
    textarea.style.position = 'fixed';
    textarea.style.opacity = '0';
    textarea.style.pointerEvents = 'none';
    document.body.appendChild(textarea);

    try {
        textarea.focus();
        textarea.select();
        textarea.setSelectionRange(0, text.length);
        if (!document.execCommand('copy')) throw new Error('Copy command was rejected');
    } finally {
        textarea.remove();
        activeElement?.focus();
    }
}

function hasActiveSync(item: CertificateItem) {
    return ['pending', 'running', 'retry'].includes(item.sync_status ?? '');
}

function CertificateStatusView({ item }: { item: CertificateItem }) {
    const retryCount =
        item.sync_count_fails !== undefined && item.sync_count_fails > 0
            ? `（已重试 ${item.sync_count_fails} 次）`
            : '';
    const content = (
        <div className="space-y-1">
            <StatusBadge
                tone={certificateStatusMeta[item.status].tone}
                pulse={item.status === 'issuing' || item.status === 'renewing'}
            >
                {certificateStatusMeta[item.status].label}
            </StatusBadge>
            {item.sync_status && item.sync_status !== 'completed' && (
                <p className="text-xs text-muted-foreground">
                    {syncStatusMeta[item.sync_status]}
                    {retryCount}
                </p>
            )}
        </div>
    );

    if (!item.last_error) return content;
    return (
        <TooltipProvider>
            <Tooltip>
                <TooltipTrigger asChild>{content}</TooltipTrigger>
                <TooltipContent className="max-w-[32rem] whitespace-pre-wrap break-words">
                    {item.last_error}
                </TooltipContent>
            </Tooltip>
        </TooltipProvider>
    );
}

function DomainBadges({ domains }: { domains: string[] }) {
    return (
        <div className="flex flex-wrap gap-1.5">
            {domains.map((domain) => (
                <Badge key={domain} variant={domain.startsWith('*.') ? 'info' : 'outline'}>
                    {domain}
                </Badge>
            ))}
        </div>
    );
}

function CreateSteps({ current }: { current: number }) {
    const steps = ['签发渠道', '域名验证', '确认申请'];
    return (
        <ol className="grid grid-cols-3" aria-label="证书申请步骤">
            {steps.map((label, index) => (
                <li key={label} className="relative flex flex-col items-center gap-2 text-center">
                    {index > 0 && (
                        <span
                            className={`absolute top-4 right-1/2 h-px w-full ${index <= current ? 'bg-primary' : 'bg-border'}`}
                        />
                    )}
                    <span
                        className={`relative z-10 flex size-8 items-center justify-center rounded-full border text-xs font-semibold ${
                            index < current
                                ? 'border-primary bg-primary text-primary-foreground'
                                : index === current
                                  ? 'border-primary bg-background text-primary'
                                  : 'border-border bg-background text-muted-foreground'
                        }`}
                    >
                        {index < current ? <Check className="size-4" /> : index + 1}
                    </span>
                    <span
                        className={`text-xs ${index === current ? 'font-medium text-foreground' : 'text-muted-foreground'}`}
                    >
                        {label}
                    </span>
                </li>
            ))}
        </ol>
    );
}

export default function CertificatePage() {
    const [certificateId, setCertificateId] = useState<string>();
    const [query, setQuery] = useState<CertificateQuery>({ page: 1, pageSize: 10 });
    const [filterKeyword, setFilterKeyword] = useState('');
    const [filterStatus, setFilterStatus] = useState<CertificateStatus | 'all'>('all');
    const [createOpen, setCreateOpen] = useState(false);
    const [createStep, setCreateStep] = useState(0);
    const [createReview, setCreateReview] = useState<CertificateCreateValues>();
    const [providersOpen, setProvidersOpen] = useState(false);
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const createForm = useForm<CertificateCreateValues>({
        resolver: zodResolver(certificateCreateSchema),
        defaultValues: createDefaults,
    });
    const createIsDirty = createForm.formState.isDirty;
    const createDomain = createForm.watch('domain');
    const createDnsZoneId = createForm.watch('dns_zone_id');
    const createAutoRenew = createForm.watch('auto_renew');

    const list = useCertificateList(query, true);
    const detail = useCertificateDetail(certificateId);
    const dnsZoneAvailability = useDnsZoneOptions({ available: true }, true);
    const providers = useCertificateProviderList(true);
    const create = useCertificateCreate();
    const save = useCertificateSave();
    const renew = useCertificateRenew();
    const remove = useCertificateDelete();
    const download = useCertificateDownload();
    const certificateRows = list.data?.list ?? [];
    const verifiedProviders =
        providers.data?.filter((provider) => provider.status === 'verified') ?? [];
    const normalizedDomain = normalizedCertificateDomain(createDomain);
    const ownerDnsZones = useDnsZoneOptions(
        { ownerOf: normalizedDomain || undefined, available: true },
        createOpen && Boolean(normalizedDomain)
    );
    const certificateSans = normalizedDomain
        ? normalizedDomain.startsWith('*.')
            ? [normalizedDomain, normalizedDomain.slice(2)]
            : [normalizedDomain]
        : [];
    const selectedDnsZone = ownerDnsZones.data?.find((dnsZone) => dnsZone.id === createDnsZoneId);
    const reviewProvider = verifiedProviders.find(
        (provider) => provider.id === createReview?.certificate_provider_id
    );
    const reviewDomain = normalizedCertificateDomain(createReview?.domain);
    const reviewSans = reviewDomain.startsWith('*.')
        ? [reviewDomain, reviewDomain.slice(2)]
        : [reviewDomain].filter(Boolean);
    const reviewZone = ownerDnsZones.data?.find(
        (dnsZone) => dnsZone.id === createReview?.dns_zone_id
    );

    useEffect(() => {
        if (!createOpen || !normalizedDomain) return;
        const matchedId = ownerDnsZones.data?.[0]?.id ?? '';
        if (createForm.getValues('dns_zone_id') !== matchedId) {
            createForm.setValue('dns_zone_id', matchedId, { shouldValidate: false });
        }
    }, [createForm, createOpen, normalizedDomain, ownerDnsZones.data]);

    function closeDetail() {
        setCertificateId(undefined);
    }

    function openCreate() {
        createForm.reset({
            ...createDefaults,
            certificate_provider_id: verifiedProviders[0]?.id ?? '',
        });
        setCreateReview(undefined);
        setCreateStep(0);
        setCreateOpen(true);
    }

    function closeCreate() {
        setCreateOpen(false);
        setCreateStep(0);
        setCreateReview(undefined);
        createForm.reset(createDefaults);
    }

    function requestCloseCreate() {
        if (create.isPending) return;
        if (!createIsDirty && createStep === 0) {
            closeCreate();
            return;
        }
        setConfirmation({
            title: '放弃证书申请草稿？',
            content: '当前申请步骤和未提交内容将被清除，此操作无法撤销。',
            confirmText: '放弃草稿',
            danger: true,
            onConfirm: closeCreate,
        });
    }

    async function continueProvider() {
        if (await createForm.trigger('certificate_provider_id')) setCreateStep(1);
    }

    async function continueDomain() {
        if (!(await createForm.trigger(['domain', 'dns_zone_id']))) return;
        setCreateReview(createForm.getValues());
        setCreateStep(2);
    }

    function submitCreate(data: CertificateCreateValues) {
        create.mutate(
            {
                domain: data.domain,
                certificate_provider_id: data.certificate_provider_id,
                dns_zone_id: data.dns_zone_id,
                config: { auto_renew: data.auto_renew },
            },
            { onSuccess: closeCreate }
        );
    }

    function requestRenew(item: CertificateItem) {
        setConfirmation({
            title: `重新签发「${item.domains[0]}」？`,
            content: '将启动新的签发流程，并沿用当前 DNS 验证与续签配置。',
            confirmText: '重新签发',
            onConfirm: () => renew.mutateAsync({ id: item.id, revision: item.revision }),
        });
    }

    function requestDelete(item: CertificateItem, closeAfterDelete = false) {
        setConfirmation({
            title: `删除证书「${item.domains[0]}」？`,
            content:
                item.website_count > 0
                    ? `当前有 ${item.website_count} 个网站引用此证书，存在引用时可能无法删除。`
                    : '此操作会删除证书记录和已保存的证书内容。',
            danger: true,
            confirmText: '删除',
            onConfirm: async () => {
                await remove.mutateAsync({ id: item.id, revision: item.revision });
                if (closeAfterDelete) closeDetail();
            },
        });
    }

    const columns: DataTableColumn<CertificateItem>[] = [
        {
            key: 'domains',
            header: '证书域名',
            cell: (item) => (
                <div className="max-w-56">
                    <DomainBadges domains={item.domains} />
                </div>
            ),
        },
        {
            key: 'issuer',
            header: '顶级发行组织',
            cell: (item) => item.issuer || '暂未签发',
        },
        {
            key: 'provider',
            header: '证书供应商',
            cell: (item) => providerTypeMeta[item.certificate_provider],
        },
        {
            key: 'not-before',
            header: '生效日期',
            cell: (item) => formatDateTime(item.not_before, '暂未签发'),
        },
        {
            key: 'expires',
            header: '有效期',
            cell: (item) =>
                item.expires_at ? (
                    <div className="space-y-0.5 whitespace-nowrap">
                        <p>{formatDateTime(item.expires_at)}</p>
                        {item.remaining_days !== undefined && (
                            <p
                                className={`text-xs ${item.remaining_days < 15 ? 'text-destructive' : 'text-muted-foreground'}`}
                            >
                                剩余 {item.remaining_days} 天
                            </p>
                        )}
                    </div>
                ) : (
                    '暂未签发'
                ),
        },
        {
            key: 'websites',
            header: '引用网站',
            cell: (item) => `${item.website_count} 个`,
        },
        {
            key: 'renewal',
            header: '自动续签',
            cell: (item) => (
                <Switch
                    size="sm"
                    checked={item.config.auto_renew}
                    aria-label={`${item.domains[0]} 自动续签`}
                    disabled={save.isPending && save.variables?.target.id === item.id}
                    onCheckedChange={(autoRenew) =>
                        save.mutate({
                            target: { id: item.id, revision: item.revision },
                            config: { auto_renew: autoRenew },
                        })
                    }
                />
            ),
        },
        {
            key: 'status',
            header: '状态',
            cell: (item) => <CertificateStatusView item={item} />,
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
                            aria-label={`打开${item.domains[0] ?? '证书'}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem onSelect={() => setCertificateId(item.id)}>
                            查看
                        </DropdownMenuItem>
                        <DropdownMenuItem
                            disabled={
                                hasActiveSync(item) ||
                                (renew.isPending && renew.variables?.id === item.id)
                            }
                            onSelect={() => requestRenew(item)}
                        >
                            续签
                        </DropdownMenuItem>
                        <DropdownMenuItem
                            disabled={!item.usable || download.isPending}
                            onSelect={() =>
                                download.mutate({
                                    id: item.id,
                                    domain: item.domains[0] ?? 'certificate',
                                })
                            }
                        >
                            下载
                        </DropdownMenuItem>
                        <DropdownMenuSeparator />
                        <DropdownMenuItem
                            variant="destructive"
                            onSelect={() => requestDelete(item)}
                        >
                            删除
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    const item = detail.data;

    return (
        <div className="mx-auto flex h-full min-h-0 w-full max-w-[1680px] flex-col gap-4 overflow-y-auto p-4 sm:p-6 md:overflow-hidden">
            <PageHeader
                title="证书管理"
                description="统一管理 ACME 账户、DNS-01 验证、证书签发和续签策略"
                actions={
                    <>
                        <Button variant="outline" onClick={() => setProvidersOpen(true)}>
                            <Settings2 />
                            证书供应商
                        </Button>
                        <Button
                            disabled={
                                !dnsZoneAvailability.data?.length || !verifiedProviders.length
                            }
                            onClick={openCreate}
                        >
                            <Plus />
                            申请证书
                        </Button>
                    </>
                }
            />

            <form
                className="flex flex-col gap-2 lg:flex-row lg:items-center"
                onSubmit={(event) => {
                    event.preventDefault();
                    setQuery((current) => ({
                        page: 1,
                        pageSize: current.pageSize ?? 10,
                        keyword: filterKeyword.trim() || undefined,
                        status: filterStatus === 'all' ? undefined : filterStatus,
                    }));
                }}
            >
                <div className="relative w-full sm:max-w-sm">
                    <Search className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-muted-foreground" />
                    <Input
                        value={filterKeyword}
                        onChange={(event) => setFilterKeyword(event.target.value)}
                        placeholder="证书域名"
                        className="pl-9"
                    />
                </div>
                <Select
                    value={filterStatus}
                    onValueChange={(value) => setFilterStatus(value as CertificateStatus | 'all')}
                >
                    <SelectTrigger className="w-full sm:w-40">
                        <SelectValue placeholder="证书状态" />
                    </SelectTrigger>
                    <SelectContent>
                        <SelectItem value="all">全部状态</SelectItem>
                        {Object.entries(certificateStatusMeta).map(([value, meta]) => (
                            <SelectItem key={value} value={value}>
                                {meta.label}
                            </SelectItem>
                        ))}
                    </SelectContent>
                </Select>
                <div className="flex flex-wrap gap-2 lg:ml-auto">
                    <Button
                        type="button"
                        variant="outline"
                        onClick={() => {
                            setFilterKeyword('');
                            setFilterStatus('all');
                            setQuery((current) => ({
                                page: 1,
                                pageSize: current.pageSize ?? 10,
                            }));
                        }}
                    >
                        <RotateCcw />
                        重置
                    </Button>
                    <Button
                        type="button"
                        variant="outline"
                        disabled={list.isFetching}
                        onClick={() => void list.refetch()}
                    >
                        <RefreshCw className={list.isFetching ? 'animate-spin' : undefined} />
                        刷新
                    </Button>
                    <Button type="submit">
                        <Search />
                        搜索
                    </Button>
                </div>
            </form>

            <div className="flex min-h-[28rem] flex-none flex-col overflow-hidden rounded-xl border bg-card md:min-h-0 md:flex-1">
                <DataTable
                    className="min-h-0 flex-1 rounded-none border-0"
                    tableClassName="min-w-[1280px]"
                    columns={columns}
                    data={certificateRows}
                    getRowKey={(certificate) => certificate.id}
                    loading={list.isLoading}
                    emptyTitle="暂无证书"
                    emptyDescription="选择可用的证书供应商和 DNS 托管域名后提交申请"
                />
                <PaginationBar
                    page={query.page ?? 1}
                    pageSize={query.pageSize ?? 10}
                    total={list.data?.total ?? 0}
                    onPageChange={(page) => setQuery((current) => ({ ...current, page }))}
                    onPageSizeChange={(pageSize) =>
                        setQuery((current) => ({ ...current, page: 1, pageSize }))
                    }
                />
            </div>

            <Sheet
                open={Boolean(certificateId)}
                onOpenChange={(nextOpen) => {
                    if (!nextOpen) closeDetail();
                }}
            >
                <SheetContent className="w-full sm:max-w-[1200px]">
                    <SheetHeader className="border-b pr-12">
                        <SheetTitle>{item ? `${item.domains[0]} 详情` : '证书详情'}</SheetTitle>
                        <SheetDescription>
                            查看证书元数据、关联网站、续签设置和当前同步状态
                        </SheetDescription>
                    </SheetHeader>
                    <div className="flex min-h-0 flex-1 flex-col gap-4 p-4">
                        {detail.isLoading ? (
                            <div className="flex h-full items-center justify-center gap-2 text-sm text-muted-foreground">
                                <Spinner />
                                正在加载证书详情
                            </div>
                        ) : item ? (
                            <>
                                <div className="flex flex-col gap-4 lg:flex-row lg:items-start lg:justify-between">
                                    <div className="min-w-0 space-y-2">
                                        <div className="flex flex-wrap items-center gap-2">
                                            <h2 className="truncate text-lg font-semibold">
                                                {item.domains[0]}
                                            </h2>
                                            <CertificateStatusView item={item} />
                                        </div>
                                        <p className="text-sm text-muted-foreground">
                                            {providerTypeMeta[item.certificate_provider]} ·{' '}
                                            {item.issuer || '暂未签发'}
                                        </p>
                                    </div>
                                    <div className="flex flex-wrap items-center gap-2">
                                        <label
                                            htmlFor="detail-auto-renew"
                                            className="flex items-center gap-2 text-sm text-muted-foreground"
                                        >
                                            自动续签
                                            <Switch
                                                id="detail-auto-renew"
                                                checked={item.config.auto_renew}
                                                disabled={save.isPending}
                                                onCheckedChange={(autoRenew) =>
                                                    save.mutate({
                                                        target: {
                                                            id: item.id,
                                                            revision: item.revision,
                                                        },
                                                        config: { auto_renew: autoRenew },
                                                    })
                                                }
                                            />
                                        </label>
                                        <Button
                                            variant="outline"
                                            size="sm"
                                            disabled={hasActiveSync(item) || renew.isPending}
                                            onClick={() => requestRenew(item)}
                                        >
                                            {renew.isPending ? <Spinner /> : <RefreshCw />}
                                            重新签发
                                        </Button>
                                        <Button
                                            size="sm"
                                            disabled={!item.usable || download.isPending}
                                            onClick={() =>
                                                download.mutate({
                                                    id: item.id,
                                                    domain: item.domains[0] ?? 'certificate',
                                                })
                                            }
                                        >
                                            {download.isPending ? <Spinner /> : <Download />}
                                            下载证书
                                        </Button>
                                        <Button
                                            variant="destructive"
                                            size="sm"
                                            disabled={remove.isPending}
                                            onClick={() => requestDelete(item, true)}
                                        >
                                            <Trash2 />
                                            删除
                                        </Button>
                                    </div>
                                </div>

                                {item.last_error && (
                                    <Alert variant="destructive">
                                        <CircleAlert />
                                        <AlertTitle>最近签发失败</AlertTitle>
                                        <AlertDescription className="whitespace-pre-wrap break-words">
                                            {item.last_error}
                                        </AlertDescription>
                                    </Alert>
                                )}

                                <div className="min-h-0 flex-1 overflow-y-auto pt-2">
                                    <DescriptionList
                                        columns={2}
                                        items={[
                                            {
                                                label: '证书域名',
                                                value: <DomainBadges domains={item.domains} />,
                                            },
                                            { label: '顶级发行组织', value: item.issuer },
                                            {
                                                label: '证书供应商',
                                                value: providerTypeMeta[item.certificate_provider],
                                            },
                                            {
                                                label: '证书状态',
                                                value: <CertificateStatusView item={item} />,
                                            },
                                            { label: '验证方式', value: 'DNS-01' },
                                            {
                                                label: 'DNS 托管域名',
                                                value: (
                                                    <Badge variant="info">
                                                        {item.dns_zone_domain}
                                                    </Badge>
                                                ),
                                            },
                                            {
                                                label: '引用网站',
                                                value: `${item.website_count} 个`,
                                            },
                                            {
                                                label: '生效时间',
                                                value: formatDateTime(item.not_before, '暂未签发'),
                                            },
                                            {
                                                label: '到期时间',
                                                value: formatDateTime(item.expires_at, '暂未签发'),
                                            },
                                            {
                                                label: '剩余有效期',
                                                value:
                                                    item.remaining_days === undefined
                                                        ? '—'
                                                        : `${item.remaining_days} 天`,
                                            },
                                            {
                                                label: '自动续签',
                                                value: item.config.auto_renew ? '已开启' : '已关闭',
                                            },
                                            {
                                                label: '序列号',
                                                value: item.serial_number ?? '—',
                                            },
                                            {
                                                label: '最近签发',
                                                value: formatDateTime(item.last_issued_at),
                                            },
                                            {
                                                label: '创建时间',
                                                value: formatDateTime(item.created_at),
                                            },
                                            {
                                                label: '更新时间',
                                                value: formatDateTime(item.updated_at),
                                            },
                                            {
                                                label: 'SHA-256 指纹',
                                                value: item.fingerprint_sha256 ? (
                                                    <div className="flex min-w-0 items-start gap-2">
                                                        <code className="min-w-0 flex-1 break-all text-xs">
                                                            {item.fingerprint_sha256}
                                                        </code>
                                                        <Button
                                                            variant="ghost"
                                                            size="icon-xs"
                                                            aria-label="复制 SHA-256 指纹"
                                                            onClick={() => {
                                                                void copyText(
                                                                    item.fingerprint_sha256 ?? ''
                                                                )
                                                                    .then(() =>
                                                                        toast.success('指纹已复制')
                                                                    )
                                                                    .catch(() =>
                                                                        toast.error(
                                                                            '复制失败，请手动复制'
                                                                        )
                                                                    );
                                                            }}
                                                        >
                                                            <Clipboard />
                                                        </Button>
                                                    </div>
                                                ) : (
                                                    '—'
                                                ),
                                            },
                                        ]}
                                    />
                                </div>
                            </>
                        ) : (
                            <EmptyState
                                title="证书不存在"
                                description="证书可能已被删除，请关闭详情后刷新列表"
                                className="h-full"
                            />
                        )}
                    </div>
                </SheetContent>
            </Sheet>

            <CertificateProviderManager
                open={providersOpen}
                onClose={() => setProvidersOpen(false)}
            />

            <Sheet
                open={createOpen}
                onOpenChange={(nextOpen) => {
                    if (!nextOpen) requestCloseCreate();
                }}
            >
                <SheetContent className="w-full sm:max-w-[560px]">
                    <SheetHeader className="border-b pr-12">
                        <SheetTitle>申请证书</SheetTitle>
                        <SheetDescription>
                            选择签发渠道，通过已托管 DNS Zone 完成 DNS-01 验证
                        </SheetDescription>
                    </SheetHeader>
                    <form
                        className="flex min-h-0 flex-1 flex-col"
                        onSubmit={createForm.handleSubmit(submitCreate)}
                    >
                        <div className="min-h-0 flex-1 space-y-6 overflow-y-auto p-4">
                            <CreateSteps current={createStep} />

                            <div className={createStep === 0 ? '' : 'hidden'}>
                                <Controller
                                    control={createForm.control}
                                    name="certificate_provider_id"
                                    render={({ field, fieldState }) => (
                                        <Field>
                                            <FieldLabel htmlFor="certificate-provider">
                                                证书供应商
                                            </FieldLabel>
                                            <Select
                                                value={field.value}
                                                onValueChange={field.onChange}
                                            >
                                                <SelectTrigger
                                                    id="certificate-provider"
                                                    className="w-full"
                                                    aria-invalid={Boolean(fieldState.error)}
                                                >
                                                    <SelectValue placeholder="选择已验证的证书供应商" />
                                                </SelectTrigger>
                                                <SelectContent>
                                                    {verifiedProviders.map((provider) => (
                                                        <SelectItem
                                                            key={provider.id}
                                                            value={provider.id}
                                                        >
                                                            {providerTypeMeta[provider.provider]}
                                                        </SelectItem>
                                                    ))}
                                                </SelectContent>
                                            </Select>
                                            <FieldDescription>
                                                仅显示凭证检测通过的供应商账号
                                            </FieldDescription>
                                            <FieldError>{fieldState.error?.message}</FieldError>
                                        </Field>
                                    )}
                                />
                            </div>

                            <div className={createStep === 1 ? '' : 'hidden'}>
                                <FieldGroup>
                                    <Field>
                                        <FieldLabel htmlFor="certificate-domain">
                                            证书域名
                                        </FieldLabel>
                                        <Input
                                            id="certificate-domain"
                                            maxLength={DOMAIN_MAX_LENGTH}
                                            placeholder="example.com 或 *.example.com"
                                            aria-invalid={Boolean(
                                                createForm.formState.errors.domain
                                            )}
                                            {...createForm.register('domain')}
                                        />
                                        <FieldError>
                                            {createForm.formState.errors.domain?.message}
                                        </FieldError>
                                    </Field>

                                    <Controller
                                        control={createForm.control}
                                        name="dns_zone_id"
                                        render={({ field, fieldState }) => (
                                            <Field>
                                                <FieldLabel htmlFor="certificate-dns-zone">
                                                    DNS 托管域名
                                                </FieldLabel>
                                                <DnsZoneSelect
                                                    id="certificate-dns-zone"
                                                    value={field.value}
                                                    onChange={field.onChange}
                                                    enabled={
                                                        createOpen && Boolean(normalizedDomain)
                                                    }
                                                    ownerOf={normalizedDomain}
                                                    requireAvailable
                                                    seedOptions={ownerDnsZones.data}
                                                    placeholder="根据证书域名自动匹配"
                                                    invalid={Boolean(fieldState.error)}
                                                />
                                                {normalizedDomain && !selectedDnsZone && (
                                                    <FieldDescription>
                                                        没有找到已同步且匹配该证书域名的托管域名
                                                    </FieldDescription>
                                                )}
                                                <FieldError>{fieldState.error?.message}</FieldError>
                                            </Field>
                                        )}
                                    />

                                    {certificateSans.length > 0 && (
                                        <Field>
                                            <FieldLabel>实际签发域名</FieldLabel>
                                            <DomainBadges domains={certificateSans} />
                                        </Field>
                                    )}
                                </FieldGroup>
                            </div>

                            <div className={createStep === 2 ? '' : 'hidden'}>
                                <div className="space-y-5">
                                    <DescriptionList
                                        columns={1}
                                        items={[
                                            {
                                                label: '证书供应商',
                                                value: reviewProvider ? (
                                                    <Badge variant="secondary">
                                                        {providerTypeMeta[reviewProvider.provider]}
                                                    </Badge>
                                                ) : (
                                                    '—'
                                                ),
                                            },
                                            {
                                                label: '签发域名',
                                                value: <DomainBadges domains={reviewSans} />,
                                            },
                                            {
                                                label: 'DNS 验证',
                                                value: reviewZone ? (
                                                    <div className="flex flex-wrap items-center gap-2">
                                                        <Badge variant="info">
                                                            {reviewZone.domain}
                                                        </Badge>
                                                        {reviewZone.dns_provider && (
                                                            <DnsProviderTag
                                                                provider={reviewZone.dns_provider}
                                                            />
                                                        )}
                                                        <Badge variant="outline">
                                                            {reviewZone.dns_provider_name}
                                                        </Badge>
                                                    </div>
                                                ) : (
                                                    '—'
                                                ),
                                            },
                                        ]}
                                    />
                                    <Controller
                                        control={createForm.control}
                                        name="auto_renew"
                                        render={({ field, fieldState }) => (
                                            <Field>
                                                <div className="flex items-center justify-between gap-4 rounded-lg border p-4">
                                                    <div>
                                                        <FieldLabel htmlFor="certificate-auto-renew">
                                                            自动续签
                                                        </FieldLabel>
                                                        <FieldDescription>
                                                            {createAutoRenew
                                                                ? '证书到期前将自动提交续签'
                                                                : '证书到期前需要手动重新签发'}
                                                        </FieldDescription>
                                                    </div>
                                                    <Switch
                                                        id="certificate-auto-renew"
                                                        checked={field.value}
                                                        onCheckedChange={field.onChange}
                                                        aria-invalid={Boolean(fieldState.error)}
                                                    />
                                                </div>
                                                <FieldError>{fieldState.error?.message}</FieldError>
                                            </Field>
                                        )}
                                    />
                                </div>
                            </div>
                        </div>

                        <SheetFooter className="border-t sm:flex-row sm:justify-end">
                            <Button
                                type="button"
                                variant="outline"
                                disabled={create.isPending}
                                onClick={requestCloseCreate}
                            >
                                取消
                            </Button>
                            {createStep > 0 && (
                                <Button
                                    type="button"
                                    variant="outline"
                                    disabled={create.isPending}
                                    onClick={() => setCreateStep((step) => Math.max(step - 1, 0))}
                                >
                                    <ArrowLeft />
                                    上一步
                                </Button>
                            )}
                            {createStep === 0 ? (
                                <Button type="button" onClick={() => void continueProvider()}>
                                    下一步
                                    <ArrowRight />
                                </Button>
                            ) : createStep === 1 ? (
                                <Button type="button" onClick={() => void continueDomain()}>
                                    下一步
                                    <ArrowRight />
                                </Button>
                            ) : (
                                <Button type="submit" disabled={create.isPending}>
                                    {create.isPending ? <Spinner /> : <Check />}
                                    提交申请
                                </Button>
                            )}
                        </SheetFooter>
                    </form>
                </SheetContent>
            </Sheet>

            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </div>
    );
}

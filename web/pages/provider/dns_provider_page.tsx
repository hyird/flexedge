import { zodResolver } from '@hookform/resolvers/zod';
import {
    CircleAlert,
    Edit,
    Globe2,
    MoreHorizontal,
    Plus,
    RefreshCw,
    RotateCcw,
    Save,
    Search,
    ShieldCheck,
    Trash2,
} from 'lucide-react';
import { useState } from 'react';
import { Controller, useForm } from 'react-hook-form';
import { useNavigate } from 'react-router-dom';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { DescriptionList } from '@/components/description_list';
import { EmptyState } from '@/components/empty_state';
import { PageHeader } from '@/components/page_header';
import { PaginationBar } from '@/components/pagination_bar';
import { StatusBadge } from '@/components/status_badge';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
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
import { Separator } from '@/components/ui/separator';
import {
    Sheet,
    SheetContent,
    SheetDescription,
    SheetFooter,
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { Spinner } from '@/components/ui/spinner';
import { Tooltip, TooltipContent, TooltipProvider, TooltipTrigger } from '@/components/ui/tooltip';
import { DNS_PROVIDER_OPTIONS, DNS_PROVIDER_REGISTRY } from '@/config/providers';
import { formatDateTime } from '@/utils/date';
import DnsProviderTag from './components/dns_provider_tag';
import {
    ACCOUNT_ID_MAX_LENGTH,
    API_TOKEN_MAX_LENGTH,
    type DnsProviderFormValues,
    dnsProviderFormSchema,
    NAME_MAX_LENGTH,
} from './dns_provider.schema';
import {
    useDnsProviderDelete,
    useDnsProviderDetail,
    useDnsProviderList,
    useDnsProviderSave,
    useDnsProviderVerify,
} from './dns_provider.service';
import type {
    DnsProviderItem,
    DnsProviderQuery,
    DnsProviderStatus,
    DnsProviderType,
} from './dns_provider.types';

const statusMeta: Record<
    DnsProviderStatus,
    { label: string; tone: 'success' | 'destructive' | 'neutral' }
> = {
    unverified: { label: '未检测', tone: 'neutral' },
    verified: { label: '凭证有效', tone: 'success' },
    invalid: { label: '凭证失效', tone: 'destructive' },
};

const emptyFormValues: DnsProviderFormValues = {
    name: '',
    provider: 'cloudflare',
    account_id: '',
    api_token: '',
};

function ProviderStatus({ item }: { item: DnsProviderItem }) {
    const badge = (
        <StatusBadge tone={statusMeta[item.status].tone}>
            {statusMeta[item.status].label}
        </StatusBadge>
    );
    const message =
        item.status === 'invalid'
            ? item.last_error
            : item.last_verified_at
              ? `最近检测：${formatDateTime(item.last_verified_at)}`
              : undefined;

    if (!message) return badge;
    return (
        <TooltipProvider>
            <Tooltip>
                <TooltipTrigger asChild>{badge}</TooltipTrigger>
                <TooltipContent className="max-w-80">{message}</TooltipContent>
            </Tooltip>
        </TooltipProvider>
    );
}

export default function DnsProviderPage() {
    const navigate = useNavigate();
    const [providerId, setProviderId] = useState<string>();
    const [formOpen, setFormOpen] = useState(false);
    const [editing, setEditing] = useState<DnsProviderItem>();
    const [filterKeyword, setFilterKeyword] = useState('');
    const [query, setQuery] = useState<DnsProviderQuery>({ page: 1, pageSize: 10 });
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const list = useDnsProviderList(query, true);
    const detail = useDnsProviderDetail(providerId);
    const save = useDnsProviderSave();
    const verify = useDnsProviderVerify();
    const remove = useDnsProviderDelete();
    const rows = list.data?.list ?? [];
    const item = detail.data;
    const form = useForm<DnsProviderFormValues>({
        resolver: zodResolver(dnsProviderFormSchema(!editing)),
        defaultValues: emptyFormValues,
    });
    const formIsDirty = form.formState.isDirty;
    const selectedProvider = form.watch('provider');
    const selectedProviderMeta = DNS_PROVIDER_REGISTRY[selectedProvider];

    function openForm(provider?: DnsProviderItem) {
        setEditing(provider);
        form.reset(
            provider
                ? {
                      name: provider.name,
                      provider: provider.provider,
                      account_id: provider.account_id,
                      api_token: '',
                  }
                : emptyFormValues
        );
        setFormOpen(true);
    }

    function closeForm() {
        setFormOpen(false);
        setEditing(undefined);
        form.reset(emptyFormValues);
    }

    function requestCloseForm() {
        if (save.isPending) return;
        if (!formIsDirty) {
            closeForm();
            return;
        }
        setConfirmation({
            title: editing ? '放弃账号修改？' : '放弃新增账号？',
            content: '当前凭据表单尚未保存，关闭后本次填写的内容将丢失。',
            confirmText: editing ? '放弃修改' : '放弃新增',
            danger: true,
            onConfirm: closeForm,
        });
    }

    function submitForm(values: DnsProviderFormValues) {
        const command = editing
            ? {
                  id: editing.id,
                  revision: editing.revision,
                  data: {
                      name: values.name,
                      ...(values.api_token ? { api_token: values.api_token } : {}),
                  },
              }
            : { data: values };
        save.mutate(command, { onSuccess: closeForm });
    }

    function requestDelete(provider: DnsProviderItem, closeDetail = false) {
        setConfirmation({
            title: `移除账号「${provider.name}」？`,
            content: '账号下仍有托管域名时不能移除。',
            danger: true,
            confirmText: '删除',
            onConfirm: async () => {
                await remove.mutateAsync({ id: provider.id, revision: provider.revision });
                if (closeDetail) setProviderId(undefined);
            },
        });
    }

    const columns: DataTableColumn<DnsProviderItem>[] = [
        {
            key: 'name',
            header: '账号说明',
            cell: (provider) => (
                <span className="block max-w-64 truncate font-medium" title={provider.name}>
                    {provider.name}
                </span>
            ),
        },
        {
            key: 'provider',
            header: '服务商',
            cell: (provider) => <DnsProviderTag provider={provider.provider} />,
        },
        {
            key: 'zones',
            header: '托管域名',
            cell: (provider) => `${provider.zone_count} 个`,
        },
        {
            key: 'status',
            header: '凭证状态',
            cell: (provider) => <ProviderStatus item={provider} />,
        },
        {
            key: 'verified',
            header: '最近检测',
            cell: (provider) => formatDateTime(provider.last_verified_at, '尚未检测'),
        },
        {
            key: 'actions',
            header: '操作',
            className: 'text-right',
            cell: (provider) => (
                <DropdownMenu>
                    <DropdownMenuTrigger asChild>
                        <Button
                            variant="ghost"
                            size="icon-sm"
                            aria-label={`打开${provider.name}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem onSelect={() => setProviderId(provider.id)}>
                            查看
                        </DropdownMenuItem>
                        <DropdownMenuItem onSelect={() => openForm(provider)}>
                            编辑
                        </DropdownMenuItem>
                        <DropdownMenuItem
                            disabled={verify.isPending && verify.variables?.id === provider.id}
                            onSelect={() =>
                                verify.mutate({ id: provider.id, revision: provider.revision })
                            }
                        >
                            检测
                        </DropdownMenuItem>
                        <DropdownMenuSeparator />
                        <DropdownMenuItem
                            variant="destructive"
                            onSelect={() => requestDelete(provider)}
                        >
                            删除
                        </DropdownMenuItem>
                    </DropdownMenuContent>
                </DropdownMenu>
            ),
        },
    ];

    return (
        <div className="flex h-full min-h-0 w-full flex-col gap-4 overflow-y-auto p-4 md:overflow-hidden">
            <PageHeader
                title="DNS服务商"
                description="管理服务商凭证，后续托管域名和节点自动解析都从这里开始"
                actions={
                    <Button onClick={() => openForm()}>
                        <Plus />
                        添加账号
                    </Button>
                }
            />

            <form
                className="flex flex-col gap-2 sm:flex-row sm:items-center"
                onSubmit={(event) => {
                    event.preventDefault();
                    setQuery((current) => ({
                        ...current,
                        page: 1,
                        keyword: filterKeyword.trim() || undefined,
                    }));
                }}
            >
                <div className="relative w-full sm:max-w-sm">
                    <Search className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-muted-foreground" />
                    <Input
                        value={filterKeyword}
                        onChange={(event) => setFilterKeyword(event.target.value)}
                        placeholder="账号名称或账户 ID"
                        className="pl-9"
                    />
                </div>
                <div className="flex flex-wrap gap-2 sm:ml-auto">
                    <Button
                        type="button"
                        variant="outline"
                        onClick={() => {
                            setFilterKeyword('');
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

            <div className="flex min-h-[28rem] flex-none flex-col overflow-hidden md:min-h-0 md:flex-1">
                <DataTable
                    className="min-h-0 flex-1 rounded-none border-0"
                    columns={columns}
                    data={rows}
                    getRowKey={(provider) => provider.id}
                    loading={list.isLoading}
                    emptyTitle="暂无 DNS 服务商账号"
                    emptyDescription="添加服务商凭证后即可托管域名"
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
                open={Boolean(providerId)}
                onOpenChange={(nextOpen) => {
                    if (!nextOpen) setProviderId(undefined);
                }}
            >
                <SheetContent className="w-full sm:max-w-[760px]">
                    <SheetHeader className="border-b pr-12">
                        <SheetTitle>{item ? `${item.name} 详情` : 'DNS服务商详情'}</SheetTitle>
                        <SheetDescription>查看凭证状态、账号信息和关联域名数量</SheetDescription>
                    </SheetHeader>
                    <div className="min-h-0 flex-1 overflow-y-auto p-4">
                        {detail.isLoading ? (
                            <div className="flex h-full items-center justify-center gap-2 text-sm text-muted-foreground">
                                <Spinner />
                                正在加载账号详情
                            </div>
                        ) : item ? (
                            <div className="space-y-5">
                                <div className="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
                                    <div className="min-w-0 space-y-2">
                                        <h2 className="truncate text-lg font-semibold">
                                            {item.name}
                                        </h2>
                                        <div className="flex flex-wrap items-center gap-2">
                                            <DnsProviderTag provider={item.provider} />
                                            <ProviderStatus item={item} />
                                        </div>
                                    </div>
                                    <div className="flex flex-wrap gap-2">
                                        <Button
                                            variant="outline"
                                            size="sm"
                                            onClick={() =>
                                                navigate(`/dns-zones?dns_provider_id=${item.id}`)
                                            }
                                        >
                                            <Globe2 />
                                            管理域名
                                        </Button>
                                        <Button
                                            variant="outline"
                                            size="sm"
                                            onClick={() => openForm(item)}
                                        >
                                            <Edit />
                                            编辑
                                        </Button>
                                        <Button
                                            size="sm"
                                            disabled={verify.isPending}
                                            onClick={() =>
                                                verify.mutate({
                                                    id: item.id,
                                                    revision: item.revision,
                                                })
                                            }
                                        >
                                            {verify.isPending ? <Spinner /> : <ShieldCheck />}
                                            检测凭证
                                        </Button>
                                    </div>
                                </div>

                                {item.last_error && (
                                    <Alert variant="destructive">
                                        <CircleAlert />
                                        <AlertTitle>凭证检测失败</AlertTitle>
                                        <AlertDescription className="whitespace-pre-wrap break-words">
                                            {item.last_error}
                                        </AlertDescription>
                                    </Alert>
                                )}

                                <DescriptionList
                                    columns={2}
                                    items={[
                                        {
                                            label: '服务商',
                                            value: <DnsProviderTag provider={item.provider} />,
                                        },
                                        { label: '账号名称', value: item.name },
                                        {
                                            label: DNS_PROVIDER_REGISTRY[item.provider]
                                                .accountLabel,
                                            value: item.account_id,
                                        },
                                        { label: '凭据摘要', value: item.token_hint },
                                        {
                                            label: '凭证状态',
                                            value: <ProviderStatus item={item} />,
                                        },
                                        { label: '托管域名', value: `${item.zone_count} 个` },
                                        {
                                            label: '最近检测',
                                            value: formatDateTime(
                                                item.last_verified_at,
                                                '尚未检测'
                                            ),
                                        },
                                        {
                                            label: '更新时间',
                                            value: formatDateTime(item.updated_at),
                                        },
                                        {
                                            label: '创建时间',
                                            value: formatDateTime(item.created_at),
                                        },
                                    ]}
                                />
                            </div>
                        ) : (
                            <EmptyState
                                title="DNS服务商账号不存在"
                                description="账号可能已被删除，请关闭详情后刷新列表"
                            />
                        )}
                    </div>
                    {item && (
                        <SheetFooter className="border-t sm:flex-row sm:justify-end">
                            <Button
                                variant="destructive"
                                disabled={remove.isPending}
                                onClick={() => requestDelete(item, true)}
                            >
                                <Trash2 />
                                删除账号
                            </Button>
                        </SheetFooter>
                    )}
                </SheetContent>
            </Sheet>

            <Sheet
                open={formOpen}
                onOpenChange={(nextOpen) => {
                    if (!nextOpen) requestCloseForm();
                }}
            >
                <SheetContent
                    className="w-full sm:max-w-[520px]"
                    onEscapeKeyDown={(event) => {
                        if (save.isPending) event.preventDefault();
                    }}
                    onPointerDownOutside={(event) => {
                        if (save.isPending) event.preventDefault();
                    }}
                >
                    <SheetHeader className="border-b pr-12">
                        <SheetTitle>
                            {editing
                                ? `更新 ${DNS_PROVIDER_REGISTRY[editing.provider].label} 账号`
                                : '添加 DNS服务商账号'}
                        </SheetTitle>
                        <SheetDescription>
                            {editing
                                ? '密钥字段留空会保留当前凭据。'
                                : '凭据保存后只会显示末尾摘要，请妥善保管原始值。'}
                        </SheetDescription>
                    </SheetHeader>
                    <form
                        id="dns-provider-form"
                        className="flex min-h-0 flex-1 flex-col"
                        onSubmit={form.handleSubmit(submitForm)}
                    >
                        <div className="min-h-0 flex-1 overflow-y-auto p-4">
                            <FieldGroup>
                                <div className="space-y-1">
                                    <h3 className="text-sm font-medium">基本信息</h3>
                                    <p className="text-xs text-muted-foreground">
                                        用于识别账号，并指定服务商侧的账户。
                                    </p>
                                </div>
                                <Field>
                                    <FieldLabel htmlFor="dns-provider-name">账号名称</FieldLabel>
                                    <Input
                                        id="dns-provider-name"
                                        maxLength={NAME_MAX_LENGTH}
                                        placeholder="例如：生产账号"
                                        aria-invalid={Boolean(form.formState.errors.name)}
                                        {...form.register('name')}
                                    />
                                    <FieldError>{form.formState.errors.name?.message}</FieldError>
                                </Field>
                                <Controller
                                    control={form.control}
                                    name="provider"
                                    render={({ field, fieldState }) => (
                                        <Field>
                                            <FieldLabel htmlFor="dns-provider-type">
                                                服务商类型
                                            </FieldLabel>
                                            <Select
                                                value={field.value}
                                                disabled={Boolean(editing)}
                                                onValueChange={(value) => {
                                                    field.onChange(value as DnsProviderType);
                                                    form.setValue('account_id', '');
                                                    form.setValue('api_token', '');
                                                }}
                                            >
                                                <SelectTrigger
                                                    id="dns-provider-type"
                                                    className="w-full"
                                                    aria-invalid={Boolean(fieldState.error)}
                                                >
                                                    <SelectValue placeholder="选择服务商" />
                                                </SelectTrigger>
                                                <SelectContent>
                                                    {DNS_PROVIDER_OPTIONS.map((option) => (
                                                        <SelectItem
                                                            key={option.value}
                                                            value={option.value}
                                                        >
                                                            {option.label}
                                                        </SelectItem>
                                                    ))}
                                                </SelectContent>
                                            </Select>
                                            <FieldError>{fieldState.error?.message}</FieldError>
                                        </Field>
                                    )}
                                />
                                <Field>
                                    <FieldLabel htmlFor="dns-provider-account">
                                        {selectedProviderMeta.accountLabel}
                                    </FieldLabel>
                                    <Input
                                        id="dns-provider-account"
                                        disabled={Boolean(editing)}
                                        maxLength={ACCOUNT_ID_MAX_LENGTH}
                                        placeholder={selectedProviderMeta.accountPlaceholder}
                                        aria-invalid={Boolean(form.formState.errors.account_id)}
                                        {...form.register('account_id')}
                                    />
                                    <FieldError>
                                        {form.formState.errors.account_id?.message}
                                    </FieldError>
                                </Field>

                                <Separator />

                                <div className="space-y-1">
                                    <h3 className="text-sm font-medium">API 参数</h3>
                                    <p className="text-xs text-muted-foreground">
                                        仅用于同步 Worker 访问服务商接口。
                                    </p>
                                </div>
                                <Field>
                                    <FieldLabel htmlFor="dns-provider-secret">
                                        {selectedProviderMeta.secretLabel}
                                    </FieldLabel>
                                    <Input
                                        id="dns-provider-secret"
                                        type="password"
                                        maxLength={API_TOKEN_MAX_LENGTH}
                                        autoComplete="new-password"
                                        aria-invalid={Boolean(form.formState.errors.api_token)}
                                        {...form.register('api_token')}
                                    />
                                    <FieldDescription>
                                        {editing
                                            ? `当前凭据 ${editing.token_hint}；不填写则保留原访问密钥`
                                            : '访问密钥保存后仅显示末尾摘要，请自行妥善保管原始值'}
                                    </FieldDescription>
                                    <FieldError>
                                        {form.formState.errors.api_token?.message}
                                    </FieldError>
                                </Field>
                            </FieldGroup>
                        </div>
                        <SheetFooter className="border-t sm:flex-row sm:justify-end">
                            <Button
                                type="button"
                                variant="outline"
                                disabled={save.isPending}
                                onClick={requestCloseForm}
                            >
                                取消
                            </Button>
                            <Button type="submit" disabled={save.isPending}>
                                {save.isPending ? <Spinner /> : <Save />}
                                保存凭证
                            </Button>
                        </SheetFooter>
                    </form>
                </SheetContent>
            </Sheet>

            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </div>
    );
}

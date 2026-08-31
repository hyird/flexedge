import { zodResolver } from '@hookform/resolvers/zod';
import { MoreHorizontal, Plus, Save } from 'lucide-react';
import { useState } from 'react';
import { Controller, useForm } from 'react-hook-form';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { DataTable, type DataTableColumn } from '@/components/data_table';
import { StatusBadge } from '@/components/status_badge';
import { Button } from '@/components/ui/button';
import {
    DropdownMenu,
    DropdownMenuContent,
    DropdownMenuItem,
    DropdownMenuSeparator,
    DropdownMenuTrigger,
} from '@/components/ui/dropdown_menu';
import {
    Dialog,
    DialogContent,
    DialogDescription,
    DialogFooter,
    DialogHeader,
    DialogTitle,
} from '@/components/ui/dialog';
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
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { Spinner } from '@/components/ui/spinner';
import { Tooltip, TooltipContent, TooltipProvider, TooltipTrigger } from '@/components/ui/tooltip';
import { CERTIFICATE_PROVIDER_OPTIONS, CERTIFICATE_PROVIDER_REGISTRY } from '@/config/providers';
import { formatDateTime } from '@/utils/date';
import {
    ACCESS_KEY_MAX_LENGTH,
    ACCOUNT_EMAIL_MAX_LENGTH,
    type CertificateProviderFormValues,
    certificateProviderFormSchema,
} from './certificate_provider.schema';
import {
    useCertificateProviderDelete,
    useCertificateProviderList,
    useCertificateProviderSave,
    useCertificateProviderVerify,
} from './certificate_provider.service';
import type {
    CertificateProviderCredentialMode,
    CertificateProviderItem,
    CertificateProviderStatus,
    CertificateProviderType,
} from './certificate_provider.types';

const providerStatusMeta: Record<
    CertificateProviderStatus,
    { label: string; tone: 'success' | 'destructive' | 'neutral' }
> = {
    unverified: { label: '未检测', tone: 'neutral' },
    verified: { label: '可用', tone: 'success' },
    invalid: { label: '不可用', tone: 'destructive' },
};

const credentialModeMeta: Record<CertificateProviderCredentialMode, string> = {
    email: '邮箱自动注册',
    access_key: 'API Access Key',
};

const emptyFormValues: CertificateProviderFormValues = {
    provider: 'zerossl',
    credential_mode: 'email',
    account_email: '',
    access_key: '',
};

function ProviderStatus({ item }: { item: CertificateProviderItem }) {
    const badge = (
        <StatusBadge tone={providerStatusMeta[item.status].tone}>
            {providerStatusMeta[item.status].label}
        </StatusBadge>
    );
    if (!item.last_error) return badge;

    return (
        <TooltipProvider>
            <Tooltip>
                <TooltipTrigger asChild>{badge}</TooltipTrigger>
                <TooltipContent className="max-w-80">{item.last_error}</TooltipContent>
            </Tooltip>
        </TooltipProvider>
    );
}

interface CertificateProviderManagerProps {
    open: boolean;
    onClose: () => void;
}

export default function CertificateProviderManager({
    open,
    onClose,
}: CertificateProviderManagerProps) {
    const [formOpen, setFormOpen] = useState(false);
    const [editingProvider, setEditingProvider] = useState<CertificateProviderItem>();
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const providers = useCertificateProviderList(open);
    const saveProvider = useCertificateProviderSave();
    const verifyProvider = useCertificateProviderVerify();
    const removeProvider = useCertificateProviderDelete();
    const form = useForm<CertificateProviderFormValues>({
        resolver: zodResolver(
            certificateProviderFormSchema(editingProvider?.credential_mode !== 'access_key')
        ),
        defaultValues: emptyFormValues,
    });
    const formIsDirty = form.formState.isDirty;
    const providerType = form.watch('provider');
    const credentialMode = form.watch('credential_mode');

    function openForm(item?: CertificateProviderItem) {
        setEditingProvider(item);
        form.reset(
            item
                ? {
                      provider: item.provider,
                      credential_mode: item.credential_mode,
                      account_email: item.account_email ?? '',
                      access_key: '',
                  }
                : emptyFormValues
        );
        setFormOpen(true);
    }

    function closeForm() {
        setFormOpen(false);
        setEditingProvider(undefined);
        form.reset(emptyFormValues);
    }

    function requestCloseForm() {
        if (saveProvider.isPending) return;
        if (!formIsDirty) {
            closeForm();
            return;
        }
        setConfirmation({
            title: editingProvider ? '放弃供应商修改？' : '放弃新增供应商？',
            content: '当前凭据表单尚未保存，关闭后本次填写的内容将丢失。',
            confirmText: editingProvider ? '放弃修改' : '放弃新增',
            danger: true,
            onConfirm: closeForm,
        });
    }

    function closeManager() {
        closeForm();
        setConfirmation(undefined);
        onClose();
    }

    function requestCloseManager() {
        if (saveProvider.isPending) return;
        if (!formOpen || !formIsDirty) {
            closeManager();
            return;
        }
        setConfirmation({
            title: '关闭证书供应商管理？',
            content: '当前凭据表单尚未保存，关闭后本次填写的内容将丢失。',
            confirmText: '放弃并关闭',
            danger: true,
            onConfirm: closeManager,
        });
    }

    function submitForm(values: CertificateProviderFormValues) {
        const credential =
            values.credential_mode === 'email'
                ? { account_email: values.account_email }
                : values.access_key
                  ? { access_key: values.access_key }
                  : {};
        const command = editingProvider
            ? {
                  id: editingProvider.id,
                  revision: editingProvider.revision,
                  data: {
                      credential_mode: values.credential_mode,
                      ...credential,
                  },
              }
            : {
                  data: {
                      provider: values.provider,
                      credential_mode: values.credential_mode,
                      ...credential,
                  },
              };
        saveProvider.mutate(command, { onSuccess: closeForm });
    }

    function requestDelete(item: CertificateProviderItem) {
        setConfirmation({
            title: `删除证书供应商「${CERTIFICATE_PROVIDER_REGISTRY[item.provider].label}」？`,
            content: '删除后，后续证书申请将不能再使用这组供应商凭据。',
            danger: true,
            confirmText: '删除',
            onConfirm: () => removeProvider.mutateAsync({ id: item.id, revision: item.revision }),
        });
    }

    const columns: DataTableColumn<CertificateProviderItem>[] = [
        {
            key: 'provider',
            header: '供应商类型',
            cell: (item) => (
                <span className="font-medium">
                    {CERTIFICATE_PROVIDER_REGISTRY[item.provider].label}
                </span>
            ),
        },
        {
            key: 'mode',
            header: '接入方式',
            cell: (item) => credentialModeMeta[item.credential_mode],
        },
        {
            key: 'credential',
            header: '账户',
            cell: (item) =>
                item.credential_mode === 'email' ? (
                    (item.account_email ?? '—')
                ) : item.access_key_hint ? (
                    <code className="rounded bg-muted px-1.5 py-0.5 text-xs">
                        {item.access_key_hint}
                    </code>
                ) : (
                    '—'
                ),
        },
        {
            key: 'status',
            header: '状态',
            cell: (item) => <ProviderStatus item={item} />,
        },
        {
            key: 'verified',
            header: '最近检测',
            cell: (item) => formatDateTime(item.last_verified_at, '尚未检测'),
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
                            aria-label={`打开${CERTIFICATE_PROVIDER_REGISTRY[item.provider].label}操作菜单`}
                        >
                            <MoreHorizontal />
                        </Button>
                    </DropdownMenuTrigger>
                    <DropdownMenuContent align="end">
                        <DropdownMenuItem onSelect={() => openForm(item)}>编辑</DropdownMenuItem>
                        <DropdownMenuItem
                            disabled={
                                verifyProvider.isPending && verifyProvider.variables?.id === item.id
                            }
                            onSelect={() =>
                                verifyProvider.mutate({ id: item.id, revision: item.revision })
                            }
                        >
                            检测
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

    return (
        <>
            <Sheet
                open={open}
                onOpenChange={(nextOpen) => {
                    if (!nextOpen) requestCloseManager();
                }}
            >
                <SheetContent className="w-full sm:max-w-[960px]">
                    <SheetHeader className="border-b pr-12">
                        <SheetTitle>证书供应商</SheetTitle>
                        <SheetDescription>管理证书签发服务的接入方式与凭据状态</SheetDescription>
                    </SheetHeader>
                    <div className="flex min-h-0 flex-1 flex-col gap-4 p-4">
                        <div className="flex justify-end">
                            <Button onClick={() => openForm()}>
                                <Plus />
                                添加供应商
                            </Button>
                        </div>
                        <div className="min-h-0 flex-1 overflow-hidden rounded-antd border bg-card">
                            <DataTable
                                className="h-full rounded-none border-0"
                                columns={columns}
                                data={providers.data ?? []}
                                getRowKey={(item) => item.id}
                                loading={providers.isLoading}
                                emptyTitle="暂无证书供应商"
                                emptyDescription="添加供应商后即可配置证书签发账号"
                            />
                        </div>
                    </div>
                </SheetContent>
            </Sheet>

            <Dialog
                open={formOpen}
                onOpenChange={(nextOpen) => {
                    if (!nextOpen) requestCloseForm();
                }}
            >
                <DialogContent
                    className="max-h-[calc(100dvh-2rem)] overflow-y-auto sm:max-w-[560px]"
                    onEscapeKeyDown={(event) => {
                        if (saveProvider.isPending) event.preventDefault();
                    }}
                    onPointerDownOutside={(event) => {
                        if (saveProvider.isPending) event.preventDefault();
                    }}
                >
                    <DialogHeader>
                        <DialogTitle>
                            {editingProvider ? '编辑证书供应商' : '添加证书供应商'}
                        </DialogTitle>
                        <DialogDescription>
                            {editingProvider?.credential_mode === 'access_key'
                                ? 'Access Key 留空会保留当前凭据。'
                                : '选择供应商支持的接入方式并填写账户信息。'}
                        </DialogDescription>
                    </DialogHeader>
                    <form onSubmit={form.handleSubmit(submitForm)}>
                        <FieldGroup>
                            <Controller
                                control={form.control}
                                name="provider"
                                render={({ field, fieldState }) => (
                                    <Field>
                                        <FieldLabel htmlFor="certificate-provider-type">
                                            供应商类型
                                        </FieldLabel>
                                        <Select
                                            value={field.value}
                                            disabled={Boolean(editingProvider)}
                                            onValueChange={(value) => {
                                                const nextProvider =
                                                    value as CertificateProviderType;
                                                const nextMode =
                                                    CERTIFICATE_PROVIDER_REGISTRY[nextProvider]
                                                        .credentialModes[0];
                                                field.onChange(nextProvider);
                                                form.setValue(
                                                    'credential_mode',
                                                    nextMode as CertificateProviderCredentialMode
                                                );
                                                form.setValue('account_email', '');
                                                form.setValue('access_key', '');
                                            }}
                                        >
                                            <SelectTrigger
                                                id="certificate-provider-type"
                                                className="w-full"
                                                aria-invalid={Boolean(fieldState.error)}
                                            >
                                                <SelectValue placeholder="选择供应商" />
                                            </SelectTrigger>
                                            <SelectContent>
                                                {CERTIFICATE_PROVIDER_OPTIONS.map((option) => (
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

                            <Controller
                                control={form.control}
                                name="credential_mode"
                                render={({ field, fieldState }) => (
                                    <Field>
                                        <FieldLabel htmlFor="certificate-provider-mode">
                                            接入方式
                                        </FieldLabel>
                                        <Select
                                            value={field.value}
                                            disabled={
                                                CERTIFICATE_PROVIDER_REGISTRY[providerType]
                                                    .credentialModes.length === 1
                                            }
                                            onValueChange={(value) => {
                                                const nextMode =
                                                    value as CertificateProviderCredentialMode;
                                                field.onChange(nextMode);
                                                form.setValue(
                                                    'account_email',
                                                    nextMode === 'email' &&
                                                        editingProvider?.credential_mode === 'email'
                                                        ? (editingProvider.account_email ?? '')
                                                        : ''
                                                );
                                                form.setValue('access_key', '');
                                            }}
                                        >
                                            <SelectTrigger
                                                id="certificate-provider-mode"
                                                className="w-full"
                                                aria-invalid={Boolean(fieldState.error)}
                                            >
                                                <SelectValue placeholder="选择接入方式" />
                                            </SelectTrigger>
                                            <SelectContent>
                                                {CERTIFICATE_PROVIDER_REGISTRY[
                                                    providerType
                                                ].credentialModes.map((mode) => (
                                                    <SelectItem key={mode} value={mode}>
                                                        {credentialModeMeta[mode]}
                                                    </SelectItem>
                                                ))}
                                            </SelectContent>
                                        </Select>
                                        <FieldError>{fieldState.error?.message}</FieldError>
                                    </Field>
                                )}
                            />

                            {credentialMode === 'email' && (
                                <Field>
                                    <FieldLabel htmlFor="certificate-provider-email">
                                        账户邮箱
                                    </FieldLabel>
                                    <Input
                                        id="certificate-provider-email"
                                        type="email"
                                        maxLength={ACCOUNT_EMAIL_MAX_LENGTH}
                                        autoComplete="email"
                                        aria-invalid={Boolean(form.formState.errors.account_email)}
                                        {...form.register('account_email')}
                                    />
                                    <FieldError>
                                        {form.formState.errors.account_email?.message}
                                    </FieldError>
                                </Field>
                            )}

                            {providerType === 'zerossl' && credentialMode === 'access_key' && (
                                <Field>
                                    <FieldLabel htmlFor="certificate-provider-key">
                                        API Access Key
                                    </FieldLabel>
                                    <Input
                                        id="certificate-provider-key"
                                        type="password"
                                        maxLength={ACCESS_KEY_MAX_LENGTH}
                                        autoComplete="new-password"
                                        placeholder={editingProvider ? '不填则不更新' : undefined}
                                        aria-invalid={Boolean(form.formState.errors.access_key)}
                                        {...form.register('access_key')}
                                    />
                                    {editingProvider?.credential_mode === 'access_key' && (
                                        <FieldDescription>
                                            当前凭据 {editingProvider.access_key_hint ?? '已保存'}
                                            ；留空则保留
                                        </FieldDescription>
                                    )}
                                    <FieldError>
                                        {form.formState.errors.access_key?.message}
                                    </FieldError>
                                </Field>
                            )}
                        </FieldGroup>

                        <DialogFooter className="mt-6">
                            <Button
                                type="button"
                                variant="outline"
                                disabled={saveProvider.isPending}
                                onClick={requestCloseForm}
                            >
                                取消
                            </Button>
                            <Button type="submit" disabled={saveProvider.isPending}>
                                {saveProvider.isPending ? <Spinner /> : <Save />}
                                保存
                            </Button>
                        </DialogFooter>
                    </form>
                </DialogContent>
            </Dialog>

            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </>
    );
}

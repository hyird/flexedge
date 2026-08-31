import { zodResolver } from '@hookform/resolvers/zod';
import { Globe2, LoaderCircle, Plus, ShieldCheck } from 'lucide-react';
import { type ReactNode, useEffect, useMemo, useState } from 'react';
import { Controller, useForm, useWatch } from 'react-hook-form';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { Alert, AlertDescription, AlertTitle } from '@/components/ui/alert';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
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
    SheetFooter,
    SheetHeader,
    SheetTitle,
} from '@/components/ui/sheet';
import { useCertificateList } from '@/pages/certificate/certificate.service';
import type { ClusterItem } from '@/pages/cluster/cluster.types';
import ClusterSelect from '@/pages/cluster/components/cluster_select';
import DnsZoneSelect, {
    type DnsZoneSelectOption,
} from '@/pages/dns_zone/components/dns_zone_select';
import {
    WEBSITE_HOSTNAME_MAX_LENGTH,
    WEBSITE_NAME_MAX_LENGTH,
    type WebsiteCreateValues,
    websiteCreateSchema,
} from '../website.schema';
import { useWebsiteCreate } from '../website.service';
import { CertificatePicker, ChoiceGroup, SectionCard, SwitchRow } from './website_form_controls';
import { createWebsiteDefaults, createWebsiteInput } from './website_helpers';

interface WebsiteCreateSheetProps {
    open: boolean;
    onClose: () => void;
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

export default function WebsiteCreateSheet({ open, onClose }: WebsiteCreateSheetProps) {
    const [selectedCluster, setSelectedCluster] = useState<ClusterItem>();
    const [selectedDnsZone, setSelectedDnsZone] = useState<DnsZoneSelectOption>();
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const create = useWebsiteCreate();
    const certificates = useCertificateList({ page: 1, pageSize: 20, usable: true }, open);
    const form = useForm<WebsiteCreateValues>({
        resolver: zodResolver(websiteCreateSchema),
        defaultValues: createWebsiteDefaults(),
    });
    const formIsDirty = form.formState.isDirty;
    const dnsMode = useWatch({ control: form.control, name: 'dns_mode' });
    const httpsEnabled = useWatch({ control: form.control, name: 'https_enabled' });

    const certificateOptions = useMemo(
        () =>
            (certificates.data?.list ?? []).map((certificate) => ({
                value: certificate.id,
                label: certificate.domains.join('、'),
                usable: certificate.usable,
            })),
        [certificates.data?.list]
    );

    useEffect(() => {
        if (!open) return;
        form.reset(createWebsiteDefaults());
        setSelectedCluster(undefined);
        setSelectedDnsZone(undefined);
        setConfirmation(undefined);
    }, [form, open]);

    const performClose = () => {
        form.reset(createWebsiteDefaults());
        setSelectedCluster(undefined);
        setSelectedDnsZone(undefined);
        setConfirmation(undefined);
        onClose();
    };

    const requestClose = () => {
        if (create.isPending) return;
        if (!formIsDirty) {
            performClose();
            return;
        }
        setConfirmation({
            title: '放弃正在创建的网站？',
            content: '当前填写内容尚未提交，关闭后将丢失。',
            confirmText: '放弃创建',
            danger: true,
            onConfirm: performClose,
        });
    };

    const submit = form.handleSubmit((data) => {
        let hostname = data.hostname.trim().toLowerCase();
        if (data.dns_mode === 'managed') {
            if (!selectedDnsZone) {
                form.setError('managed_dns_zone_id', { message: '请重新选择 DNS 托管域名' });
                return;
            }
            hostname = `${data.subdomain_prefix}.${selectedDnsZone.domain}`.toLowerCase();
        }
        create.mutate(
            {
                clusterId: data.cluster_id,
                input: createWebsiteInput(data, hostname),
            },
            { onSuccess: performClose }
        );
    });

    return (
        <>
            <Sheet open={open} onOpenChange={(nextOpen) => !nextOpen && requestClose()}>
                <SheetContent
                    showCloseButton={false}
                    className="w-full gap-0 p-0 sm:max-w-[min(780px,94vw)]"
                    onEscapeKeyDown={(event) => create.isPending && event.preventDefault()}
                    onPointerDownOutside={(event) => create.isPending && event.preventDefault()}
                >
                    <SheetHeader className="border-b px-5 py-4">
                        <SheetTitle>添加网站</SheetTitle>
                        <SheetDescription>
                            创建加速域名、初始源站和 HTTPS 配置，提交后由同步 Worker 下发至节点
                        </SheetDescription>
                    </SheetHeader>
                    <form
                        id="website-create-form"
                        noValidate
                        className="min-h-0 flex-1 space-y-5 overflow-y-auto p-4 sm:p-5"
                        onSubmit={submit}
                    >
                        <Alert>
                            <Globe2 />
                            <AlertTitle>
                                {dnsMode === 'external' ? '使用外部解析' : '使用托管解析'}
                            </AlertTitle>
                            <AlertDescription>
                                {dnsMode === 'external'
                                    ? '请自行配置 CNAME，系统将从公网 DNS 检测实际解析结果。'
                                    : '系统会自动创建 CNAME，并从公网 DNS 检测是否生效。'}
                            </AlertDescription>
                        </Alert>

                        <SectionCard
                            title="基本信息"
                            description="标识网站并选择承载流量的边缘集群"
                        >
                            <div className="grid gap-4 sm:grid-cols-2">
                                <FormControl
                                    id="create-website-name"
                                    label="网站名称"
                                    error={form.formState.errors.name?.message}
                                >
                                    <Input
                                        id="create-website-name"
                                        maxLength={WEBSITE_NAME_MAX_LENGTH}
                                        placeholder="例如官网、API 服务"
                                        aria-invalid={Boolean(form.formState.errors.name)}
                                        {...form.register('name')}
                                    />
                                </FormControl>
                            </div>
                            <Controller
                                control={form.control}
                                name="cluster_id"
                                render={({ field, fieldState }) => (
                                    <FormControl
                                        id="create-website-cluster"
                                        label="所属集群"
                                        error={fieldState.error?.message}
                                    >
                                        <ClusterSelect
                                            id="create-website-cluster"
                                            value={field.value}
                                            onChange={field.onChange}
                                            onClusterChange={setSelectedCluster}
                                            requireEnabled
                                            placeholder="选择承载网站的边缘集群"
                                            invalid={Boolean(fieldState.error)}
                                        />
                                    </FormControl>
                                )}
                            />
                        </SectionCard>

                        <SectionCard title="加速域名" description="托管解析可自动维护 CNAME 记录">
                            <Controller
                                control={form.control}
                                name="dns_mode"
                                render={({ field, fieldState }) => (
                                    <FormControl
                                        id="create-dns-mode"
                                        label="解析方式"
                                        error={fieldState.error?.message}
                                    >
                                        <ChoiceGroup
                                            id="create-dns-mode"
                                            aria-label="解析方式"
                                            value={field.value}
                                            onValueChange={(value) => {
                                                field.onChange(value);
                                                if (value === 'external') {
                                                    form.setValue(
                                                        'managed_dns_zone_id',
                                                        undefined,
                                                        { shouldDirty: true }
                                                    );
                                                    setSelectedDnsZone(undefined);
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
                            {dnsMode === 'managed' ? (
                                <div className="grid gap-4 sm:grid-cols-2">
                                    <Controller
                                        control={form.control}
                                        name="managed_dns_zone_id"
                                        render={({ field, fieldState }) => (
                                            <FormControl
                                                id="create-dns-zone"
                                                label="DNS 托管域名"
                                                error={fieldState.error?.message}
                                            >
                                                <DnsZoneSelect
                                                    id="create-dns-zone"
                                                    value={field.value}
                                                    onChange={field.onChange}
                                                    onDnsZoneChange={setSelectedDnsZone}
                                                    requireAvailable
                                                    placeholder="选择用于自动解析的托管域名"
                                                    invalid={Boolean(fieldState.error)}
                                                />
                                            </FormControl>
                                        )}
                                    />
                                    <FormControl
                                        id="create-subdomain-prefix"
                                        label="子域名"
                                        error={form.formState.errors.subdomain_prefix?.message}
                                        description={
                                            selectedDnsZone
                                                ? `完整域名：${form.watch('subdomain_prefix') || 'www'}.${selectedDnsZone.domain}`
                                                : undefined
                                        }
                                    >
                                        <Input
                                            id="create-subdomain-prefix"
                                            placeholder="www"
                                            aria-invalid={Boolean(
                                                form.formState.errors.subdomain_prefix
                                            )}
                                            {...form.register('subdomain_prefix')}
                                        />
                                    </FormControl>
                                </div>
                            ) : (
                                <FormControl
                                    id="create-hostname"
                                    label="加速域名"
                                    error={form.formState.errors.hostname?.message}
                                >
                                    <Input
                                        id="create-hostname"
                                        maxLength={WEBSITE_HOSTNAME_MAX_LENGTH}
                                        placeholder="www.example.com 或 *.example.com"
                                        aria-invalid={Boolean(form.formState.errors.hostname)}
                                        {...form.register('hostname')}
                                    />
                                </FormControl>
                            )}
                            <div className="flex flex-wrap items-center gap-2 rounded-lg border bg-muted/30 px-3 py-2 text-sm">
                                <span className="text-muted-foreground">
                                    {dnsMode === 'external'
                                        ? '请手动配置 CNAME：'
                                        : '系统将维护 CNAME：'}
                                </span>
                                <Badge variant="info" className="font-mono">
                                    {selectedCluster?.access_domain ?? '请先选择所属集群'}
                                </Badge>
                            </div>
                        </SectionCard>

                        <SectionCard
                            title="初始源站"
                            description="创建后可继续添加主源站或备用源站"
                        >
                            <div className="grid gap-4 sm:grid-cols-[140px_minmax(0,1fr)_140px]">
                                <Controller
                                    control={form.control}
                                    name="origin_protocol"
                                    render={({ field, fieldState }) => (
                                        <FormControl
                                            id="create-origin-protocol"
                                            label="协议"
                                            error={fieldState.error?.message}
                                        >
                                            <Select
                                                value={field.value}
                                                onValueChange={(value: 'http' | 'https') => {
                                                    field.onChange(value);
                                                    form.setValue(
                                                        'origin_port',
                                                        value === 'https' ? 443 : 80,
                                                        { shouldDirty: true, shouldValidate: true }
                                                    );
                                                }}
                                            >
                                                <SelectTrigger
                                                    id="create-origin-protocol"
                                                    className="w-full"
                                                >
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
                                    id="create-origin-host"
                                    label="地址"
                                    error={form.formState.errors.origin_host?.message}
                                >
                                    <Input
                                        id="create-origin-host"
                                        placeholder="10.0.0.1 或 origin.example.com"
                                        aria-invalid={Boolean(form.formState.errors.origin_host)}
                                        {...form.register('origin_host')}
                                    />
                                </FormControl>
                                <FormControl
                                    id="create-origin-port"
                                    label="端口"
                                    error={form.formState.errors.origin_port?.message}
                                >
                                    <Input
                                        id="create-origin-port"
                                        type="number"
                                        min={1}
                                        max={65535}
                                        aria-invalid={Boolean(form.formState.errors.origin_port)}
                                        {...form.register('origin_port', { valueAsNumber: true })}
                                    />
                                </FormControl>
                            </div>
                            <FormControl
                                id="create-origin-host-header"
                                label="回源 Host"
                                error={form.formState.errors.origin_host_header?.message}
                                description="留空时使用实际访问域名"
                            >
                                <Input
                                    id="create-origin-host-header"
                                    placeholder="留空则使用访问域名"
                                    aria-invalid={Boolean(form.formState.errors.origin_host_header)}
                                    {...form.register('origin_host_header')}
                                />
                            </FormControl>
                        </SectionCard>

                        <SectionCard title="HTTPS" description="证书按域名自动匹配，创建后仍可调整">
                            <Controller
                                control={form.control}
                                name="https_enabled"
                                render={({ field }) => (
                                    <SwitchRow
                                        id="create-https-enabled"
                                        label="启用 HTTPS"
                                        checked={field.value}
                                        onCheckedChange={field.onChange}
                                    />
                                )}
                            />
                            <Controller
                                control={form.control}
                                name="certificate_ids"
                                render={({ field, fieldState }) => (
                                    <FormControl
                                        id="create-certificates"
                                        label="绑定证书（可多选）"
                                        error={fieldState.error?.message}
                                    >
                                        <CertificatePicker
                                            value={field.value}
                                            options={certificateOptions}
                                            onChange={field.onChange}
                                            loading={
                                                certificates.isLoading || certificates.isFetching
                                            }
                                            disabled={!httpsEnabled}
                                            invalid={Boolean(fieldState.error)}
                                        />
                                    </FormControl>
                                )}
                            />
                            <div className="grid gap-3 sm:grid-cols-2">
                                <Controller
                                    control={form.control}
                                    name="minimum_tls_version"
                                    render={({ field }) => (
                                        <FormControl id="create-minimum-tls" label="最低 TLS 版本">
                                            <Select
                                                value={field.value}
                                                onValueChange={field.onChange}
                                                disabled={!httpsEnabled}
                                            >
                                                <SelectTrigger
                                                    id="create-minimum-tls"
                                                    className="w-full"
                                                >
                                                    <SelectValue />
                                                </SelectTrigger>
                                                <SelectContent>
                                                    <SelectItem value="1.2">
                                                        TLS 1.2（推荐）
                                                    </SelectItem>
                                                    <SelectItem value="1.3">TLS 1.3</SelectItem>
                                                </SelectContent>
                                            </Select>
                                        </FormControl>
                                    )}
                                />
                                <Controller
                                    control={form.control}
                                    name="force_https"
                                    render={({ field }) => (
                                        <SwitchRow
                                            id="create-force-https"
                                            label="自动跳转 HTTPS"
                                            checked={field.value}
                                            onCheckedChange={field.onChange}
                                            disabled={!httpsEnabled}
                                        />
                                    )}
                                />
                                <Controller
                                    control={form.control}
                                    name="http2_enabled"
                                    render={({ field }) => (
                                        <SwitchRow
                                            id="create-http2-enabled"
                                            label="HTTP/2"
                                            checked={field.value}
                                            onCheckedChange={field.onChange}
                                            disabled={!httpsEnabled}
                                        />
                                    )}
                                />
                                <Controller
                                    control={form.control}
                                    name="hsts_enabled"
                                    render={({ field }) => (
                                        <SwitchRow
                                            id="create-hsts-enabled"
                                            label="HSTS"
                                            checked={field.value}
                                            onCheckedChange={field.onChange}
                                            disabled={!httpsEnabled}
                                        />
                                    )}
                                />
                            </div>
                            {!httpsEnabled && (
                                <Alert>
                                    <ShieldCheck />
                                    <AlertTitle>当前仅启用 HTTP</AlertTitle>
                                    <AlertDescription>
                                        可先完成创建，后续在网站配置中绑定证书并开启 HTTPS。
                                    </AlertDescription>
                                </Alert>
                            )}
                        </SectionCard>
                    </form>
                    <SheetFooter className="flex-row justify-end border-t bg-background px-5 py-3">
                        <Button
                            variant="outline"
                            disabled={create.isPending}
                            onClick={requestClose}
                        >
                            取消
                        </Button>
                        <Button
                            type="submit"
                            form="website-create-form"
                            disabled={create.isPending}
                        >
                            {create.isPending ? (
                                <LoaderCircle className="animate-spin" />
                            ) : (
                                <Plus />
                            )}
                            创建网站
                        </Button>
                    </SheetFooter>
                </SheetContent>
            </Sheet>
            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </>
    );
}

import { zodResolver } from '@hookform/resolvers/zod';
import {
    ChevronLeft,
    ChevronRight,
    Globe2,
    Plus,
    Save,
    Server,
    Settings2,
    Trash2,
} from 'lucide-react';
import { useEffect, useState } from 'react';
import { Controller, useForm } from 'react-hook-form';
import ConfirmDrawer, { type ConfirmDrawerAction } from '@/components/ConfirmDrawer';
import { EmptyState } from '@/components/empty_state';
import { PageHeader } from '@/components/page_header';
import { StatusBadge } from '@/components/status_badge';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
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
import { cn } from '@/lib/utils';
import DnsZoneSelect, {
    type DnsZoneSelectOption,
} from '@/pages/dns_zone/components/dns_zone_select';
import {
    CLUSTER_NAME_MAX_LENGTH,
    type ClusterFormValues,
    clusterFormSchema,
    HOSTNAME_PREFIX_MAX_LENGTH,
} from './cluster.schema';
import { useClusterDelete, useClusterList, useClusterSave } from './cluster.service';
import type { ClusterItem, SaveClusterDto } from './cluster.types';
import NodePanel from './node/node_panel';

const emptyValues: ClusterFormValues = {
    name: '',
    dns_zone_id: '',
    hostname_prefix: '',
    status: 'enabled',
};

export default function ClusterPage() {
    const [open, setOpen] = useState(false);
    const [clusterPage, setClusterPage] = useState(1);
    const [editing, setEditing] = useState<ClusterItem>();
    const [selectedZone, setSelectedZone] = useState<DnsZoneSelectOption>();
    const [selectedClusterId, setSelectedClusterId] = useState<string>();
    const [nodeCreateRequest, setNodeCreateRequest] = useState(0);
    const [confirmation, setConfirmation] = useState<ConfirmDrawerAction>();
    const list = useClusterList({ page: clusterPage, pageSize: 50 }, true);
    const save = useClusterSave();
    const remove = useClusterDelete();
    const form = useForm<ClusterFormValues>({
        resolver: zodResolver(clusterFormSchema),
        defaultValues: emptyValues,
    });
    const formIsDirty = form.formState.isDirty;
    const selectedZoneId = form.watch('dns_zone_id');
    const hostnamePrefix = form.watch('hostname_prefix');
    const accessDomain =
        hostnamePrefix?.trim() && selectedZone && selectedZone.id === selectedZoneId
            ? `${hostnamePrefix.trim().toLowerCase()}.${selectedZone.domain}`
            : '—';
    const clusters = list.data?.list ?? [];
    const selectedCluster = clusters.find((item) => item.id === selectedClusterId);
    const totalPages = list.data?.totalPages ?? 1;

    useEffect(() => {
        if (clusters.some((item) => item.id === selectedClusterId)) return;
        setSelectedClusterId(clusters[0]?.id);
    }, [clusters, selectedClusterId]);

    useEffect(() => {
        const availablePages = list.data?.totalPages ?? 0;
        if (availablePages > 0 && clusterPage > availablePages) setClusterPage(availablePages);
    }, [clusterPage, list.data?.totalPages]);

    const openForm = (item?: ClusterItem) => {
        setEditing(item);
        if (item) {
            setSelectedZone({
                id: item.dns_zone_id,
                domain: item.dns_zone_domain,
                dns_provider_name: item.dns_provider_name,
            });
            form.reset({
                name: item.name,
                dns_zone_id: item.dns_zone_id,
                hostname_prefix: item.hostname_prefix,
                status: item.status,
            });
        } else {
            setSelectedZone(undefined);
            form.reset(emptyValues);
        }
        setOpen(true);
    };

    const closeForm = () => {
        if (save.isPending || remove.isPending) return;
        setOpen(false);
        setEditing(undefined);
        setSelectedZone(undefined);
        form.reset(emptyValues);
    };

    const requestCloseForm = () => {
        if (save.isPending || remove.isPending) return;
        if (!formIsDirty) {
            closeForm();
            return;
        }
        setConfirmation({
            title: editing ? '放弃未保存的集群修改？' : '放弃正在创建的集群？',
            content: '当前配置尚未保存，关闭后本次填写的内容将丢失。',
            confirmText: editing ? '放弃修改' : '放弃创建',
            danger: true,
            onConfirm: closeForm,
        });
    };

    const submit = form.handleSubmit((values) => {
        const payload: SaveClusterDto = {
            ...values,
            name: values.name.trim(),
            hostname_prefix: values.hostname_prefix.trim().toLowerCase(),
        };
        save.mutate(
            editing
                ? { id: editing.id, revision: editing.revision, data: payload }
                : { data: payload },
            { onSuccess: closeForm }
        );
    });

    return (
        <div className="mx-auto flex h-full w-full max-w-[1680px] flex-col gap-4 overflow-y-auto p-4 sm:p-6 md:overflow-hidden">
            <PageHeader
                eyebrow="基础设施"
                title="边缘集群"
                description="集中管理节点、接入域名和 DNS 投影；配置变更通过可靠任务下发。"
                actions={
                    <Button onClick={() => openForm()}>
                        <Plus />
                        创建集群
                    </Button>
                }
            />

            <div className="flex min-h-0 flex-none flex-col gap-4 md:flex-1 lg:flex-row">
                <aside className="flex max-h-52 shrink-0 flex-col overflow-hidden rounded-xl border bg-card lg:max-h-none lg:w-56">
                    <div className="flex items-center justify-between border-b px-4 py-3">
                        <span className="text-xs font-semibold uppercase tracking-wider text-muted-foreground">
                            集群列表
                        </span>
                        <Badge variant="secondary">{list.data?.total ?? 0}</Badge>
                    </div>
                    <nav
                        className="min-h-0 flex-1 space-y-1 overflow-y-auto p-2"
                        aria-label="集群列表"
                    >
                        {clusters.map((cluster) => {
                            const healthy =
                                cluster.node_count > 0 &&
                                cluster.online_node_count === cluster.node_count;
                            return (
                                <button
                                    key={cluster.id}
                                    type="button"
                                    className={cn(
                                        'flex w-full items-center gap-3 rounded-lg px-3 py-2.5 text-left text-sm transition-colors hover:bg-accent',
                                        selectedClusterId === cluster.id &&
                                            'bg-accent font-medium text-accent-foreground'
                                    )}
                                    onClick={() => setSelectedClusterId(cluster.id)}
                                >
                                    <span
                                        className={cn(
                                            'size-2 shrink-0 rounded-full bg-slate-300',
                                            healthy && 'bg-emerald-500'
                                        )}
                                    />
                                    <span className="min-w-0 flex-1 truncate">{cluster.name}</span>
                                    <span className="text-xs text-muted-foreground tabular-nums">
                                        {cluster.online_node_count}/{cluster.node_count}
                                    </span>
                                </button>
                            );
                        })}
                        {!list.isLoading && clusters.length === 0 && (
                            <EmptyState title="暂无集群" className="min-h-36 px-3 py-6" />
                        )}
                    </nav>
                    {(list.data?.total ?? 0) > 50 && (
                        <div className="flex items-center justify-between border-t px-3 py-2 text-xs text-muted-foreground">
                            <Button
                                variant="ghost"
                                size="icon-xs"
                                aria-label="上一页集群"
                                disabled={clusterPage <= 1}
                                onClick={() => setClusterPage((page) => page - 1)}
                            >
                                <ChevronLeft />
                            </Button>
                            <span className="tabular-nums">
                                {clusterPage} / {totalPages}
                            </span>
                            <Button
                                variant="ghost"
                                size="icon-xs"
                                aria-label="下一页集群"
                                disabled={clusterPage >= totalPages}
                                onClick={() => setClusterPage((page) => page + 1)}
                            >
                                <ChevronRight />
                            </Button>
                        </div>
                    )}
                </aside>

                {selectedCluster ? (
                    <section className="flex min-h-0 min-w-0 flex-none flex-col gap-4 md:flex-1">
                        <div className="rounded-xl border bg-card p-4 shadow-xs">
                            <div className="flex flex-col gap-4 xl:flex-row xl:items-center">
                                <div className="flex min-w-0 items-center gap-3">
                                    <div className="flex size-10 shrink-0 items-center justify-center rounded-xl bg-primary/10 text-primary">
                                        <Server className="size-5" />
                                    </div>
                                    <div className="min-w-0">
                                        <div className="flex items-center gap-2">
                                            <h2 className="truncate text-lg font-semibold">
                                                {selectedCluster.name}
                                            </h2>
                                            <StatusBadge
                                                tone={
                                                    selectedCluster.status === 'enabled'
                                                        ? 'success'
                                                        : 'neutral'
                                                }
                                            >
                                                {selectedCluster.status === 'enabled'
                                                    ? '启用'
                                                    : '停用'}
                                            </StatusBadge>
                                        </div>
                                        <p className="mt-1 text-xs text-muted-foreground">
                                            {selectedCluster.online_node_count}/
                                            {selectedCluster.node_count} 个节点在线
                                        </p>
                                    </div>
                                </div>
                                <div className="grid min-w-0 flex-1 gap-2 text-sm sm:grid-cols-3 xl:ml-4">
                                    <div className="min-w-0 rounded-lg bg-muted/60 px-3 py-2">
                                        <p className="text-xs text-muted-foreground">接入域名</p>
                                        <p className="mt-0.5 truncate font-mono text-xs">
                                            {selectedCluster.access_domain}
                                        </p>
                                    </div>
                                    <div className="min-w-0 rounded-lg bg-muted/60 px-3 py-2">
                                        <p className="text-xs text-muted-foreground">
                                            DNS 托管域名
                                        </p>
                                        <p className="mt-0.5 truncate">
                                            {selectedCluster.dns_zone_domain}
                                        </p>
                                    </div>
                                    <div className="min-w-0 rounded-lg bg-muted/60 px-3 py-2">
                                        <p className="text-xs text-muted-foreground">DNS 服务商</p>
                                        <p className="mt-0.5 truncate">
                                            {selectedCluster.dns_provider_name}
                                        </p>
                                    </div>
                                </div>
                                <div className="flex shrink-0 gap-2">
                                    <Button
                                        variant="outline"
                                        onClick={() => openForm(selectedCluster)}
                                    >
                                        <Settings2 />
                                        集群设置
                                    </Button>
                                    <Button
                                        onClick={() => setNodeCreateRequest((value) => value + 1)}
                                    >
                                        <Plus />
                                        添加节点
                                    </Button>
                                </div>
                            </div>
                        </div>
                        <NodePanel cluster={selectedCluster} createRequest={nodeCreateRequest} />
                    </section>
                ) : (
                    <div className="flex flex-1 items-center justify-center rounded-xl border border-dashed bg-card">
                        <EmptyState
                            title="还没有边缘集群"
                            description="创建集群后即可添加节点并生成网站接入域名。"
                            icon={<Globe2 className="size-5" />}
                            action={<Button onClick={() => openForm()}>创建第一个集群</Button>}
                        />
                    </div>
                )}
            </div>

            <Sheet open={open} onOpenChange={(nextOpen) => !nextOpen && requestCloseForm()}>
                <SheetContent className="w-full gap-0 p-0 sm:max-w-[min(36rem,calc(100vw-2rem))]">
                    <SheetHeader className="border-b px-4 py-4 pr-12 sm:px-6 sm:py-5">
                        <SheetTitle>{editing ? '集群设置' : '新建集群'}</SheetTitle>
                        <SheetDescription>
                            配置节点所属边界以及网站 CNAME 使用的接入域名。
                        </SheetDescription>
                    </SheetHeader>
                    <form
                        id="cluster-form"
                        className="min-h-0 flex-1 overflow-y-auto p-4 sm:p-6"
                        onSubmit={submit}
                    >
                        <FieldGroup>
                            <Field data-invalid={Boolean(form.formState.errors.name)}>
                                <FieldLabel htmlFor="cluster-name">集群名称</FieldLabel>
                                <Input
                                    id="cluster-name"
                                    maxLength={CLUSTER_NAME_MAX_LENGTH}
                                    placeholder="华南集群"
                                    aria-invalid={Boolean(form.formState.errors.name)}
                                    {...form.register('name')}
                                />
                                <FieldError>{form.formState.errors.name?.message}</FieldError>
                            </Field>
                            <div className="grid gap-4 sm:grid-cols-2">
                                <Field
                                    data-invalid={Boolean(form.formState.errors.hostname_prefix)}
                                >
                                    <FieldLabel htmlFor="cluster-hostname-prefix">
                                        主机前缀
                                    </FieldLabel>
                                    <Input
                                        id="cluster-hostname-prefix"
                                        maxLength={HOSTNAME_PREFIX_MAX_LENGTH}
                                        placeholder="cn-south"
                                        aria-invalid={Boolean(
                                            form.formState.errors.hostname_prefix
                                        )}
                                        {...form.register('hostname_prefix')}
                                    />
                                    <FieldError>
                                        {form.formState.errors.hostname_prefix?.message}
                                    </FieldError>
                                </Field>
                                <Controller
                                    control={form.control}
                                    name="dns_zone_id"
                                    render={({ field, fieldState }) => (
                                        <Field data-invalid={fieldState.invalid}>
                                            <FieldLabel htmlFor="cluster-dns-zone">
                                                DNS 托管域名
                                            </FieldLabel>
                                            <DnsZoneSelect
                                                id="cluster-dns-zone"
                                                value={field.value}
                                                onChange={field.onChange}
                                                enabled={open}
                                                seedOptions={selectedZone ? [selectedZone] : []}
                                                onDnsZoneChange={setSelectedZone}
                                                invalid={fieldState.invalid}
                                            />
                                            <FieldError>{fieldState.error?.message}</FieldError>
                                        </Field>
                                    )}
                                />
                            </div>
                            <div className="rounded-lg border bg-muted/40 px-3 py-2.5">
                                <p className="text-xs text-muted-foreground">网站 CNAME 接入目标</p>
                                <p className="mt-1 break-all font-mono text-sm font-medium text-primary tabular-nums">
                                    {accessDomain}
                                </p>
                            </div>
                            <Controller
                                control={form.control}
                                name="status"
                                render={({ field, fieldState }) => (
                                    <Field data-invalid={fieldState.invalid}>
                                        <FieldLabel>集群状态</FieldLabel>
                                        <Select value={field.value} onValueChange={field.onChange}>
                                            <SelectTrigger
                                                className="w-full"
                                                aria-invalid={fieldState.invalid}
                                            >
                                                <SelectValue />
                                            </SelectTrigger>
                                            <SelectContent>
                                                <SelectItem value="enabled">启用</SelectItem>
                                                <SelectItem value="disabled">停用</SelectItem>
                                            </SelectContent>
                                        </Select>
                                        <FieldDescription>
                                            停用后不会改变已保存配置，但不再作为可用交付集群。
                                        </FieldDescription>
                                        <FieldError>{fieldState.error?.message}</FieldError>
                                    </Field>
                                )}
                            />
                        </FieldGroup>
                        {editing && (
                            <div className="mt-8">
                                <Separator className="mb-5" />
                                <div className="rounded-lg border border-destructive/25 bg-destructive/5 p-4">
                                    <h3 className="text-sm font-medium text-destructive">
                                        危险操作
                                    </h3>
                                    <p className="mt-1 text-xs text-muted-foreground">
                                        删除前必须迁移或删除集群中的全部节点和网站。
                                    </p>
                                    <Button
                                        variant="destructive"
                                        size="sm"
                                        className="mt-4"
                                        disabled={remove.isPending}
                                        onClick={() =>
                                            setConfirmation({
                                                title: `删除集群「${editing.name}」？`,
                                                content:
                                                    '删除前必须先迁移或删除集群中的全部节点和网站。',
                                                danger: true,
                                                confirmText: '删除集群',
                                                onConfirm: async () => {
                                                    await remove.mutateAsync({
                                                        id: editing.id,
                                                        revision: editing.revision,
                                                    });
                                                    closeForm();
                                                },
                                            })
                                        }
                                    >
                                        <Trash2 />
                                        删除集群
                                    </Button>
                                </div>
                            </div>
                        )}
                    </form>
                    <SheetFooter className="shrink-0 flex-row justify-end border-t px-4 py-4 sm:px-6">
                        <Button
                            variant="outline"
                            disabled={save.isPending}
                            onClick={requestCloseForm}
                        >
                            取消
                        </Button>
                        <Button type="submit" form="cluster-form" disabled={save.isPending}>
                            <Save />
                            {save.isPending ? '正在保存' : '保存配置'}
                        </Button>
                    </SheetFooter>
                </SheetContent>
            </Sheet>
            <ConfirmDrawer action={confirmation} onClose={() => setConfirmation(undefined)} />
        </div>
    );
}

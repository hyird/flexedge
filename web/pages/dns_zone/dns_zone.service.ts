import { skipToken, useQuery, useQueryClient } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { taskQueryKeys } from '@/pages/task/task.types';
import type { RevisionedResourceRef } from '@/utils/http';
import {
    createDnsZone,
    getAvailableDnsZones,
    getDnsZone,
    getDnsZoneOptions,
    getDnsZones,
    refreshDnsLines,
    removeDnsZone,
    saveDnsZone,
    syncDnsZone,
} from './dns_zone.api';
import { selectDnsLines } from './dns_zone.selectors';
import {
    type DnsZoneQuery,
    type DnsZoneOptionQuery,
    type DnsZoneConfig,
    dnsZoneQueryKeys,
    type SyncDnsZoneCommand,
} from './dns_zone.types';

export function useAvailableDnsZones(providerId?: string) {
    return useQuery({
        queryKey: dnsZoneQueryKeys.available(providerId),
        queryFn: providerId ? () => getAvailableDnsZones(providerId) : skipToken,
    });
}

export function useDnsZoneList(query: DnsZoneQuery, enabled = true) {
    return useQuery({
        queryKey: [...dnsZoneQueryKeys.lists(), query],
        queryFn: () => getDnsZones(query),
        enabled,
        refetchInterval: (state) =>
            state.state.data?.list.some((item) => item.sync_status === 'pending') ? 1000 : false,
    });
}

export function useDnsZoneOptions(query: DnsZoneOptionQuery, enabled = true) {
    return useQuery({
        queryKey: dnsZoneQueryKeys.options(query),
        queryFn: () => getDnsZoneOptions(query),
        enabled,
    });
}

export function useDnsZoneDetail(id: string | undefined) {
    return useQuery({
        queryKey: dnsZoneQueryKeys.detail(id ?? ''),
        queryFn: id ? () => getDnsZone(id) : skipToken,
        refetchInterval: (state) => (state.state.data?.sync_status === 'pending' ? 1000 : false),
    });
}

export function useDnsZoneCreate() {
    return useMutationWithMessage({
        mutationFn: createDnsZone,
        successMessage: '域名已保存，正在从服务商同步解析记录',
        invalidateKeys: [dnsZoneQueryKeys.all, taskQueryKeys.all],
    });
}

export function useDnsZoneSync() {
    return useMutationWithMessage({
        mutationFn: ({ id, conflictPolicy }: SyncDnsZoneCommand) => syncDnsZone(id, conflictPolicy),
        successMessage: '同步任务已提交',
        invalidateKeys: [dnsZoneQueryKeys.all, taskQueryKeys.all],
    });
}

export function useDnsZoneDelete() {
    return useMutationWithMessage({
        mutationFn: removeDnsZone,
        successMessage: '托管域名已移除',
        invalidateKeys: [dnsZoneQueryKeys.all, taskQueryKeys.all],
    });
}

export function useDnsLineList(dnsZoneId: string | undefined, enabled = true) {
    return useQuery({
        queryKey: dnsZoneQueryKeys.lines(dnsZoneId),
        queryFn: dnsZoneId ? async () => selectDnsLines(await getDnsZone(dnsZoneId)) : skipToken,
        enabled: enabled && Boolean(dnsZoneId),
        refetchInterval: enabled && dnsZoneId ? 5000 : false,
    });
}

export function useDnsLineRefresh() {
    const queryClient = useQueryClient();
    return useMutationWithMessage({
        mutationFn: refreshDnsLines,
        successMessage: 'DNS 线路刷新任务已提交',
        invalidateKeys: [taskQueryKeys.all],
        onSuccess: (_, dnsZoneId) => {
            void queryClient.invalidateQueries({
                queryKey: dnsZoneQueryKeys.detail(dnsZoneId),
            });
            void queryClient.invalidateQueries({ queryKey: dnsZoneQueryKeys.lines(dnsZoneId) });
        },
    });
}

export function useDnsZoneSave() {
    return useMutationWithMessage({
        mutationFn: ({
            target,
            config,
        }: {
            target: RevisionedResourceRef;
            config: DnsZoneConfig;
        }) => saveDnsZone(target, config),
        successMessage: 'DNS 配置已保存，同步任务已提交',
        invalidateKeys: [dnsZoneQueryKeys.all, taskQueryKeys.all],
    });
}

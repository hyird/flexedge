import { skipToken, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { taskQueryKeys } from '@/pages/task/task.types';
import {
    createDnsProvider,
    getDnsProvider,
    getDnsProviders,
    removeDnsProvider,
    updateDnsProvider,
    verifyDnsProvider,
} from './dns_provider.api';
import {
    type DnsProviderQuery,
    dnsProviderQueryKeys,
    type SaveDnsProviderCommand,
} from './dns_provider.types';

export function useDnsProviderList(query?: DnsProviderQuery, enabled = true) {
    return useQuery({
        queryKey: dnsProviderQueryKeys.list({ ...(query ?? {}) }),
        queryFn: () => getDnsProviders(query),
        enabled,
    });
}

export function useDnsProviderDetail(id: string | undefined) {
    return useQuery({
        queryKey: dnsProviderQueryKeys.detail(id ?? ''),
        queryFn: id ? () => getDnsProvider(id) : skipToken,
    });
}

export function useDnsProviderSave() {
    return useMutationWithMessage({
        mutationFn: (command: SaveDnsProviderCommand) =>
            'id' in command && command.id !== undefined
                ? updateDnsProvider(command, command.data)
                : createDnsProvider(command.data),
        successMessage: (_, command) =>
            command.id ? 'DNS服务商账号已更新' : 'DNS服务商账号已添加',
        invalidateKeys: [dnsProviderQueryKeys.all, taskQueryKeys.all],
    });
}

export function useDnsProviderVerify() {
    return useMutationWithMessage({
        mutationFn: verifyDnsProvider,
        successMessage: 'DNS服务商凭据检测任务已提交',
        invalidateKeys: [dnsProviderQueryKeys.all, taskQueryKeys.all],
        invalidateOnError: true,
    });
}

export function useDnsProviderDelete() {
    return useMutationWithMessage({
        mutationFn: removeDnsProvider,
        successMessage: 'DNS服务商账号已移除',
        invalidateKeys: [dnsProviderQueryKeys.all, taskQueryKeys.all],
    });
}

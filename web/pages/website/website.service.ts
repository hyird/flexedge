import { skipToken, useQuery } from '@tanstack/react-query';
import { useCallback, useEffect, useRef, useState } from 'react';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { clusterQueryKeys } from '@/pages/cluster/cluster.types';
import { dnsZoneQueryKeys } from '@/pages/dns_zone/dns_zone.types';
import { taskQueryKeys } from '@/pages/task/task.types';
import { decodeLogTailEvent, mergeLogTail } from '@/utils/log_tail';
import {
    createWebsite,
    getWebsite,
    getWebsiteAccessLogStreamUrl,
    getWebsites,
    removeWebsite,
    saveWebsite,
} from './website.api';
import type {
    WebsiteAccessLog,
    WebsiteAccessLogLimit,
    WebsiteAccessLogTail,
    WebsiteItem,
    WebsiteQuery,
    WebsiteSaveInput,
    WebsiteSaveTarget,
} from './website.types';
import { websiteQueryKeys } from './website.types';

const websiteMutationInvalidations = [
    websiteQueryKeys.all,
    clusterQueryKeys.all,
    dnsZoneQueryKeys.all,
    taskQueryKeys.all,
];

export function useWebsiteList(query: WebsiteQuery, enabled = true) {
    return useQuery({
        queryKey: websiteQueryKeys.list({ ...query }),
        queryFn: () => getWebsites(query),
        enabled,
        refetchInterval: (state) => {
            const list = state.state.data?.list as WebsiteItem[] | undefined;
            return list?.some(
                (item) => item.deploy_status === 'pending' || item.deploy_status === 'partial'
            )
                ? 3000
                : false;
        },
    });
}

export function useWebsiteDetail(id?: string, enabled = true) {
    return useQuery({
        queryKey: websiteQueryKeys.detail(id ?? ''),
        queryFn: id ? () => getWebsite(id) : skipToken,
        enabled: enabled && id !== undefined,
        staleTime: 30_000,
        refetchInterval: enabled && id !== undefined ? 5000 : false,
    });
}

export function useWebsiteAccessLogs(
    id?: string,
    limit: WebsiteAccessLogLimit = 100,
    enabled = true
) {
    const [data, setData] = useState<WebsiteAccessLogTail>();
    const [isLoading, setIsLoading] = useState(false);
    const [isFetching, setIsFetching] = useState(false);
    const [refreshToken, setRefreshToken] = useState(0);
    const streamKeyRef = useRef('');
    const refetch = useCallback(() => setRefreshToken((value) => value + 1), []);

    useEffect(() => {
        void refreshToken;
        if (!enabled || !id) {
            streamKeyRef.current = '';
            setData(undefined);
            setIsLoading(false);
            setIsFetching(false);
            return;
        }

        const streamKey = `${id}:${limit}`;
        if (streamKeyRef.current !== streamKey) {
            streamKeyRef.current = streamKey;
            setData(undefined);
            setIsLoading(true);
        } else {
            setIsFetching(true);
        }

        const source = new EventSource(getWebsiteAccessLogStreamUrl(id, limit), {
            withCredentials: true,
        });
        source.addEventListener('logs', (event) => {
            const update = decodeLogTailEvent<WebsiteAccessLog>(event as MessageEvent<string>);
            if (!update) {
                setIsLoading(false);
                setIsFetching(false);
                return;
            }
            setData((current) => {
                return mergeLogTail(current, update, limit);
            });
            setIsLoading(false);
            setIsFetching(false);
        });
        source.addEventListener('error', () => {
            setIsLoading(false);
            setIsFetching(false);
        });
        return () => source.close();
    }, [enabled, id, limit, refreshToken]);

    return { data, isLoading, isFetching, refetch };
}

export function useWebsiteCreate() {
    return useMutationWithMessage({
        mutationFn: ({ clusterId, input }: { clusterId: string; input: WebsiteSaveInput }) =>
            createWebsite(clusterId, input),
        successMessage: '网站已创建',
        invalidateKeys: websiteMutationInvalidations,
    });
}

export function useWebsiteSave() {
    return useMutationWithMessage({
        mutationFn: ({ target, input }: { target: WebsiteSaveTarget; input: WebsiteSaveInput }) =>
            saveWebsite(target, input),
        successMessage: '网站配置已保存',
        invalidateKeys: websiteMutationInvalidations,
    });
}

export function useWebsiteDelete() {
    return useMutationWithMessage({
        mutationFn: removeWebsite,
        successMessage: '网站已删除',
        invalidateKeys: websiteMutationInvalidations,
    });
}

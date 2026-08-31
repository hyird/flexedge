import { useQuery } from '@tanstack/react-query';
import { useCallback, useEffect, useRef, useState } from 'react';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { dnsZoneQueryKeys } from '@/pages/dns_zone/dns_zone.types';
import { taskQueryKeys } from '@/pages/task/task.types';
import { decodeLogTailEvent, mergeLogTail } from '@/utils/log_tail';
import { clusterQueryKeys } from '../cluster.types';
import {
    createNode,
    getNodeCredentials,
    getNodeLogStreamUrl,
    getNodes,
    removeNode,
    resetNodeCredentials,
    saveNode,
} from './node.api';
import {
    type NodeLog,
    type NodeLogLimit,
    type NodeLogTail,
    type NodeQuery,
    type NodeSaveCommand,
    nodeQueryKeys,
} from './node.types';

async function persistNode(command: NodeSaveCommand) {
    return command.target ? saveNode(command.target, command.input) : createNode(command.input);
}

export function useNodeList(query?: NodeQuery, enabled = true) {
    return useQuery({
        queryKey: nodeQueryKeys.list({ ...(query ?? {}) }),
        queryFn: () => getNodes(query),
        enabled,
    });
}

export function useNodeCredentials(id?: string, enabled = true) {
    return useQuery({
        queryKey: [...nodeQueryKeys.detail(id ?? ''), 'credentials'] as const,
        queryFn: () => getNodeCredentials(id ?? ''),
        enabled: enabled && Boolean(id),
        retry: false,
        gcTime: 0,
    });
}

export function useNodeLogs(id?: string, limit: NodeLogLimit = 100, enabled = true) {
    const [data, setData] = useState<NodeLogTail>();
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

        const source = new EventSource(getNodeLogStreamUrl(id, limit), {
            withCredentials: true,
        });
        source.addEventListener('logs', (event) => {
            const update = decodeLogTailEvent<NodeLog>(event as MessageEvent<string>);
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

export function useNodeSave() {
    return useMutationWithMessage({
        mutationFn: persistNode,
        successMessage: (_, command) => (command.target ? '节点配置已更新' : '节点已创建'),
        invalidateKeys: [
            nodeQueryKeys.all,
            clusterQueryKeys.all,
            dnsZoneQueryKeys.all,
            taskQueryKeys.all,
        ],
    });
}

export function useNodeDelete() {
    return useMutationWithMessage({
        mutationFn: removeNode,
        successMessage: '节点已删除',
        invalidateKeys: [
            nodeQueryKeys.all,
            clusterQueryKeys.all,
            dnsZoneQueryKeys.all,
            taskQueryKeys.all,
        ],
    });
}

export function useNodeCredentialsReset() {
    return useMutationWithMessage({
        mutationFn: resetNodeCredentials,
        successMessage: '节点凭据已重置',
        invalidateKeys: [
            nodeQueryKeys.all,
            clusterQueryKeys.all,
            dnsZoneQueryKeys.all,
            taskQueryKeys.all,
        ],
    });
}

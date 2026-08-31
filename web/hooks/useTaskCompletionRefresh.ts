import { useEffect, useRef } from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { certificateQueryKeys } from '@/pages/certificate/certificate.types';
import { clusterQueryKeys } from '@/pages/cluster/cluster.types';
import { nodeQueryKeys } from '@/pages/cluster/node/node.types';
import { dnsZoneQueryKeys } from '@/pages/dns_zone/dns_zone.types';
import { certificateProviderQueryKeys } from '@/pages/provider/certificate_provider.types';
import { dnsProviderQueryKeys } from '@/pages/provider/dns_provider.types';
import { useTaskList } from '@/pages/task/task.service';
import type { TaskItem, TaskStatus } from '@/pages/task/task.types';
import { websiteQueryKeys } from '@/pages/website/website.types';

const activeStatuses = new Set<TaskStatus>(['pending', 'running', 'retry']);

function queryKeysFor(item: TaskItem) {
    if (item.kind === 'provider')
        return [dnsProviderQueryKeys.all, certificateProviderQueryKeys.all, dnsZoneQueryKeys.all];
    if (item.kind === 'dns')
        return [
            dnsZoneQueryKeys.all,
            clusterQueryKeys.all,
            nodeQueryKeys.all,
            websiteQueryKeys.all,
        ];
    if (item.kind === 'certificate') return [certificateQueryKeys.all, websiteQueryKeys.all];
    return [websiteQueryKeys.all, clusterQueryKeys.all, dnsZoneQueryKeys.all];
}

export function useTaskCompletionRefresh() {
    const queryClient = useQueryClient();
    const tasks = useTaskList({ page: 1, pageSize: 100 }, true, 1000);
    const previousStatuses = useRef(new Map<string, TaskStatus>());
    const hasReceivedSnapshot = useRef(false);

    useEffect(() => {
        if (!tasks.data) return;
        const nextStatuses = new Map<string, TaskStatus>();
        for (const task of tasks.data.list) {
            const previous = previousStatuses.current.get(task.id);
            const completedSincePreviousSnapshot =
                (!hasReceivedSnapshot.current ||
                    (previous !== undefined && activeStatuses.has(previous))) &&
                !activeStatuses.has(task.status);
            if (completedSincePreviousSnapshot) {
                for (const queryKey of queryKeysFor(task)) {
                    void queryClient.invalidateQueries({ queryKey });
                }
            }
            nextStatuses.set(task.id, task.status);
        }
        previousStatuses.current = nextStatuses;
        hasReceivedSnapshot.current = true;
    }, [queryClient, tasks.data]);
}

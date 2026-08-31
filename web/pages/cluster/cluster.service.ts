import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { taskQueryKeys } from '@/pages/task/task.types';
import { createCluster, getClusters, removeCluster, updateCluster } from './cluster.api';
import { type ClusterQuery, clusterQueryKeys, type SaveClusterCommand } from './cluster.types';

export function useClusterList(query?: ClusterQuery, enabled = true) {
    return useQuery({
        queryKey: clusterQueryKeys.list({ ...(query ?? {}) }),
        queryFn: () => getClusters(query),
        enabled,
    });
}

export function useClusterSave() {
    return useMutationWithMessage({
        mutationFn: (command: SaveClusterCommand) =>
            command.id !== undefined
                ? updateCluster(command, command.data)
                : createCluster(command.data),
        successMessage: (_, command) => (command.id ? '集群配置已更新' : '集群已创建'),
        invalidateKeys: [clusterQueryKeys.all, taskQueryKeys.all],
    });
}

export function useClusterDelete() {
    return useMutationWithMessage({
        mutationFn: removeCluster,
        successMessage: '集群已删除',
        invalidateKeys: [clusterQueryKeys.all, taskQueryKeys.all],
    });
}

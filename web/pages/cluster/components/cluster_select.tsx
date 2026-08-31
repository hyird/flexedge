import { useDeferredValue, useEffect, useMemo, useState } from 'react';
import { Combobox } from '@/components/ui/combobox';
import { useClusterList } from '../cluster.service';
import type { ClusterItem } from '../cluster.types';

interface ClusterSelectProps {
    value?: string;
    onChange?: (value: string) => void;
    enabled?: boolean;
    requireEnabled?: boolean;
    seedOptions?: ClusterItem[];
    onClusterChange?: (cluster: ClusterItem | undefined) => void;
    placeholder?: string;
    disabled?: boolean;
    invalid?: boolean;
    className?: string;
    id?: string;
    name?: string;
    onBlur?: () => void;
    'aria-describedby'?: string;
}

export default function ClusterSelect({
    value,
    onChange,
    enabled = true,
    requireEnabled = false,
    seedOptions = [],
    onClusterChange,
    placeholder = '选择集群',
    disabled,
    invalid,
    className,
    id,
    name,
    onBlur,
    'aria-describedby': ariaDescribedBy,
}: ClusterSelectProps) {
    const [search, setSearch] = useState('');
    const [knownClusters, setKnownClusters] = useState<ClusterItem[]>([]);
    const keyword = useDeferredValue(search.trim());
    const query = useClusterList({ page: 1, pageSize: 20, keyword: keyword || undefined }, enabled);

    useEffect(() => {
        if (!query.data?.list.length) return;
        setKnownClusters((current) => {
            const values = new Map(current.map((cluster) => [cluster.id, cluster]));
            for (const cluster of query.data?.list ?? []) values.set(cluster.id, cluster);
            return [...values.values()];
        });
    }, [query.data?.list]);

    const clusters = useMemo(() => {
        const values = new Map(
            (query.data?.list ?? []).map((cluster) => [cluster.id, cluster] as const)
        );
        for (const cluster of seedOptions) values.set(cluster.id, cluster);
        const selectedCluster = knownClusters.find((cluster) => cluster.id === value);
        if (selectedCluster) values.set(selectedCluster.id, selectedCluster);
        return [...values.values()];
    }, [knownClusters, query.data?.list, seedOptions, value]);

    return (
        <Combobox
            id={id}
            name={name}
            value={value}
            options={clusters.map((cluster) => ({
                value: cluster.id,
                label: cluster.name,
                description: `${cluster.access_domain} · ${cluster.online_node_count}/${cluster.node_count} 节点在线`,
                disabled: requireEnabled && cluster.status !== 'enabled',
            }))}
            onSearchChange={setSearch}
            onValueChange={(nextValue) => {
                onChange?.(nextValue);
                onClusterChange?.(clusters.find((cluster) => cluster.id === nextValue));
            }}
            placeholder={placeholder}
            searchPlaceholder="搜索集群…"
            emptyText="没有匹配的集群"
            loading={query.isLoading || query.isFetching}
            disabled={disabled || !enabled}
            invalid={invalid}
            aria-describedby={ariaDescribedBy}
            className={className}
            onBlur={onBlur}
        />
    );
}

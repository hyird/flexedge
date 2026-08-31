import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';

export type ClusterStatus = 'enabled' | 'disabled';

export interface ClusterItem {
    id: string;
    name: string;
    dns_zone_id: string;
    dns_zone_domain: string;
    dns_provider_name: string;
    hostname_prefix: string;
    access_domain: string;
    node_count: number;
    online_node_count: number;
    status: ClusterStatus;
    revision: number;
    created_at: string;
    updated_at: string;
}

export interface SaveClusterDto {
    name: string;
    dns_zone_id: string;
    hostname_prefix: string;
    status: ClusterStatus;
}

export interface ClusterQuery extends PageParams {
    dnsZoneId?: string;
    status?: ClusterStatus;
}

export type SaveClusterCommand =
    | { id?: undefined; data: SaveClusterDto }
    | { id: string; revision: number; data: SaveClusterDto };

export const clusterQueryKeys = createQueryKeys('clusters');

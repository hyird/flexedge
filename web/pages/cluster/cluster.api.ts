import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import type { PaginatedResponse } from '@/utils/pagination.types';
import { appendQueryParams } from '@/utils/query.params';
import type { ClusterItem, ClusterQuery, SaveClusterDto } from './cluster.types';

const BASE = '/api/clusters';

export async function getClusters(query?: ClusterQuery) {
    return normalizePaginatedResponse(
        await request.get<PaginatedResponse<ClusterItem>>(appendQueryParams(BASE, query))
    );
}

export function createCluster(data: SaveClusterDto) {
    return request.postOperation(BASE, data);
}

export function updateCluster(target: RevisionedResourceRef, data: SaveClusterDto) {
    return request.putOperation(
        `${BASE}/${target.id}`,
        data,
        withExpectedRevision(target.revision)
    );
}

export function removeCluster(target: RevisionedResourceRef) {
    return request.deleteOperation(`${BASE}/${target.id}`, withExpectedRevision(target.revision));
}

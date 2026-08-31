import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import type { PaginatedResponse } from '@/utils/pagination.types';
import { appendQueryParams } from '@/utils/query.params';
import type {
    WebsiteAccessLogLimit,
    WebsiteConfig,
    WebsiteItem,
    WebsiteQuery,
    WebsiteSaveInput,
    WebsiteSaveTarget,
    WebsiteWire,
} from './website.types';

const BASE = '/api/websites';

function firstDomain(config: WebsiteConfig) {
    return config.domains[0]?.hostname ?? '';
}

function decorateWebsite(website: WebsiteWire): WebsiteItem {
    return {
        ...website,
        website_name: website.config.name?.trim() || firstDomain(website.config),
        origin_count: website.config.origins.length,
        https_enabled: website.config.https_enabled,
        deploy_status: website.runtime.deploy_status,
        target_node_count: website.runtime.target_node_count,
        synced_node_count: website.runtime.synced_node_count,
    };
}

export async function getWebsites(query?: WebsiteQuery) {
    const result = normalizePaginatedResponse(
        await request.get<PaginatedResponse<WebsiteWire>>(appendQueryParams(BASE, query))
    );
    return { ...result, list: result.list.map(decorateWebsite) };
}

export async function getWebsite(id: string) {
    return decorateWebsite(await request.get<WebsiteWire>(`${BASE}/${id}`));
}

export function getWebsiteAccessLogStreamUrl(
    id: string,
    limit: WebsiteAccessLogLimit,
    after?: string
) {
    return appendQueryParams(`${BASE}/${id}/access-logs/stream`, { limit, after });
}

export function createWebsite(clusterId: string, input: WebsiteSaveInput) {
    return request.postOperation(appendQueryParams(BASE, { clusterId }), input);
}

export function saveWebsite(target: WebsiteSaveTarget, input: WebsiteSaveInput) {
    return request.putOperation(
        appendQueryParams(`${BASE}/${target.id}`, { clusterId: target.cluster_id }),
        input,
        withExpectedRevision(target.revision)
    );
}

export function removeWebsite(target: RevisionedResourceRef) {
    return request.deleteOperation(`${BASE}/${target.id}`, withExpectedRevision(target.revision));
}

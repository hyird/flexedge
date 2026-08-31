import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import type { PaginatedResponse } from '@/utils/pagination.types';
import { appendQueryParams } from '@/utils/query.params';
import type {
    CreateDnsProviderDto,
    DnsProviderItem,
    DnsProviderQuery,
    UpdateDnsProviderDto,
} from './dns_provider.types';

const DNS_PROVIDERS = '/api/providers/dns';

export async function getDnsProviders(query?: DnsProviderQuery) {
    return normalizePaginatedResponse(
        await request.get<PaginatedResponse<DnsProviderItem>>(
            appendQueryParams(DNS_PROVIDERS, query)
        )
    );
}

export function getDnsProvider(id: string) {
    return request.get<DnsProviderItem>(`${DNS_PROVIDERS}/${id}`);
}

export function createDnsProvider(data: CreateDnsProviderDto) {
    return request.postOperation(DNS_PROVIDERS, data);
}

export function updateDnsProvider(target: RevisionedResourceRef, data: UpdateDnsProviderDto) {
    return request.putOperation(
        `${DNS_PROVIDERS}/${target.id}`,
        data,
        withExpectedRevision(target.revision)
    );
}

export function verifyDnsProvider(target: RevisionedResourceRef) {
    return request.postOperation(
        `${DNS_PROVIDERS}/${target.id}/verify`,
        undefined,
        withExpectedRevision(target.revision)
    );
}

export function removeDnsProvider(target: RevisionedResourceRef) {
    return request.deleteOperation(
        `${DNS_PROVIDERS}/${target.id}`,
        withExpectedRevision(target.revision)
    );
}

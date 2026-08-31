import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import type { PaginatedResponse } from '@/utils/pagination.types';
import { appendQueryParams } from '@/utils/query.params';
import type {
    AvailableDnsZoneItem,
    CreateDnsZoneDto,
    DnsSyncConflictPolicy,
    DnsZoneConfig,
    DnsZoneItem,
    DnsZoneOption,
    DnsZoneOptionQuery,
    DnsZoneQuery,
    DnsZoneWire,
} from './dns_zone.types';

const BASE = '/api/dns-zones';

function decorateDnsZone(dnsZone: DnsZoneWire): DnsZoneItem {
    return {
        ...dnsZone,
        record_count: dnsZone.config.records.length + dnsZone.runtime.projected_records.length,
    };
}

export async function getAvailableDnsZones(providerId: string) {
    return (
        await request.get<{ list: AvailableDnsZoneItem[] }>(
            appendQueryParams(`${BASE}/available`, { dnsProviderId: providerId })
        )
    ).list;
}

export async function getDnsZones(query?: DnsZoneQuery) {
    const result = normalizePaginatedResponse(
        await request.get<PaginatedResponse<DnsZoneWire>>(appendQueryParams(BASE, query))
    );
    return { ...result, list: result.list.map(decorateDnsZone) };
}

export async function getDnsZoneOptions(query: DnsZoneOptionQuery) {
    return (
        await request.get<{ list: DnsZoneOption[] }>(appendQueryParams(`${BASE}/options`, query))
    ).list;
}

export async function getDnsZone(id: string) {
    return decorateDnsZone(await request.get<DnsZoneWire>(`${BASE}/${id}`));
}

export function createDnsZone(data: CreateDnsZoneDto) {
    return request.postOperation(BASE, data);
}

export function syncDnsZone(id: string, conflictPolicy?: DnsSyncConflictPolicy) {
    return request.postOperation(
        `${BASE}/${id}/sync`,
        conflictPolicy ? { conflict_policy: conflictPolicy } : {}
    );
}

export function removeDnsZone(target: RevisionedResourceRef) {
    return request.deleteOperation(`${BASE}/${target.id}`, withExpectedRevision(target.revision));
}

export function refreshDnsLines(dnsZoneId: string) {
    return syncDnsZone(dnsZoneId);
}

export function saveDnsZone(target: RevisionedResourceRef, config: DnsZoneConfig) {
    return request.putOperation(
        `${BASE}/${target.id}`,
        config,
        withExpectedRevision(target.revision)
    );
}

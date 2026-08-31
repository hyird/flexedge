import type { DnsProviderCode } from '@/config/providers';
import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';

export type SyncStatus = 'pending' | 'synced' | 'failed' | 'conflict';
export type DnsSyncConflictPolicy = 'local' | 'remote';
export type DnsProviderType = DnsProviderCode;
export type DnsRecordType = 'A' | 'AAAA' | 'CNAME' | 'TXT' | 'MX';

export interface AvailableDnsZoneItem {
    domain: string;
    status: 'active';
}

export interface DnsZoneWire {
    id: string;
    dns_provider_id: string;
    dns_provider: DnsProviderType;
    dns_provider_name: string;
    domain: string;
    sync_status: SyncStatus;
    revision: number;
    desired_revision: number;
    synced_revision: number;
    website_count: number;
    last_synced_at?: string;
    last_error?: string;
    created_at: string;
    updated_at: string;
    config: DnsZoneConfig;
    runtime: DnsZoneRuntime;
}

export interface DnsZoneItem extends DnsZoneWire {
    record_count: number;
}

export interface DnsZoneConfig {
    records: DnsRecordConfig[];
}

export interface DnsRecordConfig {
    id: string;
    type: DnsRecordType;
    name: string;
    content: string;
    ttl: number;
    priority?: number;
    proxied: boolean;
    line_code: string;
}

export interface DnsZoneRuntime {
    records_imported: boolean;
    lines_synced_at?: string;
    lines: Array<{
        code: string;
        name: string;
        display_name: string;
        status: 'enabled' | 'disabled';
    }>;
    projected_records: DnsRecordConfig[];
    record_states: Array<{
        id: string;
        sync_status: SyncStatus;
        synced_revision: number;
        last_error?: string;
    }>;
    conflicts: Array<{
        id: string;
        type: DnsRecordType;
        name: string;
        line_code: string;
        local_content: string;
        remote_content: string;
    }>;
}

export interface DnsZoneQuery extends PageParams {
    dnsProviderId?: string;
}

export interface DnsZoneOption {
    id: string;
    domain: string;
    dns_provider: DnsProviderType;
    dns_provider_name: string;
    sync_status: SyncStatus;
    available: boolean;
}

export interface DnsZoneOptionQuery {
    keyword?: string;
    ownerOf?: string;
    available?: boolean;
}

export interface CreateDnsZoneDto {
    dns_provider_id: string;
    domain: string;
}

export interface SyncDnsZoneCommand {
    id: string;
    conflictPolicy?: DnsSyncConflictPolicy;
}

export interface DnsRecordItem extends DnsRecordConfig {
    line_name: string;
    managed: boolean;
    sync_status: SyncStatus;
    last_error?: string;
}

export type DnsRecordQuery = PageParams;

export interface DnsLineItem {
    line_code: string;
    line_name: string;
    line_display_name: string;
    status: 'enabled' | 'disabled';
    last_synced_at?: string;
}

export const dnsZoneQueryKeys = {
    ...createQueryKeys('dns-zones'),
    available: (providerId?: string) => ['dns-zones', 'available', providerId] as const,
    lines: (dnsZoneId?: string) => ['dns-zones', 'lines', dnsZoneId] as const,
    options: (query: DnsZoneOptionQuery) => ['dns-zones', 'options', query] as const,
};

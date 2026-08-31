import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';
import type { DnsProviderCode } from '@/config/providers';

export type DnsProviderStatus = 'unverified' | 'verified' | 'invalid';
export type DnsProviderType = DnsProviderCode;

export interface DnsProviderItem {
    id: string;
    revision: number;
    name: string;
    account_id: string;
    provider: DnsProviderType;
    token_hint: string;
    status: DnsProviderStatus;
    zone_count: number;
    last_verified_at?: string;
    last_error?: string;
    created_at: string;
    updated_at: string;
}

export interface CreateDnsProviderDto {
    name: string;
    provider: DnsProviderType;
    account_id: string;
    api_token: string;
}

export interface UpdateDnsProviderDto {
    name: string;
    api_token?: string;
}

export interface DnsProviderQuery extends PageParams {
    status?: DnsProviderStatus;
}

export type SaveDnsProviderCommand =
    | { id?: undefined; data: CreateDnsProviderDto }
    | { id: string; revision: number; data: UpdateDnsProviderDto };

export const dnsProviderQueryKeys = createQueryKeys('providers:dns');

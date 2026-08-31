import type { PageParams } from '@/utils/pagination.types';
import { createQueryKeys } from '@/utils/query.keys';
import type { CertificateProviderType } from '@/pages/provider/certificate_provider.types';

export type CertificateStatus = 'pending' | 'issuing' | 'valid' | 'renewing' | 'failed' | 'expired';
export type CertificateSyncStatus = 'pending' | 'running' | 'retry' | 'completed';
export interface CertificateItem {
    id: string;
    revision: number;
    domains: string[];
    issuer: string;
    certificate_provider_id: string;
    certificate_provider: CertificateProviderType;
    status: CertificateStatus;
    usable: boolean;
    dns_zone_id: string;
    dns_zone_domain: string;
    not_before?: string;
    expires_at?: string;
    remaining_days?: number;
    last_error?: string;
    serial_number?: string;
    fingerprint_sha256?: string;
    last_issued_at?: string;
    sync_status?: CertificateSyncStatus;
    sync_count_fails?: number;
    website_count: number;
    created_at: string;
    updated_at: string;
    config: CertificateConfig;
}

export interface CertificateConfig {
    auto_renew: boolean;
}

export interface CertificateQuery extends PageParams {
    status?: CertificateStatus;
    usable?: boolean;
}

export interface CertificateCreateInput {
    domain: string;
    certificate_provider_id: string;
    dns_zone_id: string;
    config: CertificateConfig;
}

export interface DownloadCertificateCommand {
    id: string;
    domain: string;
}

export const certificateQueryKeys = createQueryKeys('certificates');

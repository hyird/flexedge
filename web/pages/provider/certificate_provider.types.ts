import type { CertificateProviderCode } from '@/config/providers';
import { createQueryKeys } from '@/utils/query.keys';

export type CertificateProviderType = CertificateProviderCode;
export type CertificateProviderCredentialMode = 'email' | 'access_key';
export type CertificateProviderStatus = 'unverified' | 'verified' | 'invalid';

export interface CertificateProviderItem {
    id: string;
    revision: number;
    provider: CertificateProviderType;
    credential_mode: CertificateProviderCredentialMode;
    account_email?: string;
    access_key_hint?: string;
    status: CertificateProviderStatus;
    last_verified_at?: string;
    last_error?: string;
    created_at: string;
    updated_at: string;
}

export interface CreateCertificateProviderDto {
    provider: CertificateProviderType;
    credential_mode: CertificateProviderCredentialMode;
    account_email?: string;
    access_key?: string;
}

export interface UpdateCertificateProviderDto {
    credential_mode: CertificateProviderCredentialMode;
    account_email?: string;
    access_key?: string;
}

export type SaveCertificateProviderCommand =
    | { data: CreateCertificateProviderDto; id?: undefined }
    | { id: string; revision: number; data: UpdateCertificateProviderDto };

export const certificateProviderQueryKeys = createQueryKeys('providers:certificate');

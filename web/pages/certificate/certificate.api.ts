import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import { normalizePaginatedResponse } from '@/utils/pagination.response';
import type { PaginatedResponse } from '@/utils/pagination.types';
import { appendQueryParams } from '@/utils/query.params';
import type {
    CertificateConfig,
    CertificateItem,
    CertificateQuery,
    CertificateCreateInput,
} from './certificate.types';

const BASE = '/api/certificates';

export async function getCertificates(query?: CertificateQuery) {
    return normalizePaginatedResponse(
        await request.get<PaginatedResponse<CertificateItem>>(appendQueryParams(BASE, query))
    );
}

export function getCertificate(id: string) {
    return request.get<CertificateItem>(`${BASE}/${id}`);
}

export function createCertificate(input: CertificateCreateInput) {
    return request.postOperation(BASE, input);
}

export function saveCertificate(target: RevisionedResourceRef, config: CertificateConfig) {
    return request.putOperation(
        `${BASE}/${target.id}`,
        config,
        withExpectedRevision(target.revision)
    );
}

export function renewCertificate(target: RevisionedResourceRef) {
    return request.postOperation(
        `${BASE}/${target.id}/renew`,
        undefined,
        withExpectedRevision(target.revision)
    );
}

export function removeCertificate(target: RevisionedResourceRef) {
    return request.deleteOperation(`${BASE}/${target.id}`, withExpectedRevision(target.revision));
}

export function downloadCertificate(id: string) {
    return request.getRaw<Blob>(`${BASE}/${id}/download`, { responseType: 'blob' });
}

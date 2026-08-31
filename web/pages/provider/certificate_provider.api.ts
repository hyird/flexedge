import request, { type RevisionedResourceRef, withExpectedRevision } from '@/utils/http';
import type {
    CertificateProviderItem,
    CreateCertificateProviderDto,
    UpdateCertificateProviderDto,
} from './certificate_provider.types';

const CERTIFICATE_PROVIDERS = '/api/providers/certificate';

export function getCertificateProviders() {
    return request.get<CertificateProviderItem[]>(CERTIFICATE_PROVIDERS);
}

export function createCertificateProvider(data: CreateCertificateProviderDto) {
    return request.postOperation(CERTIFICATE_PROVIDERS, data);
}

export function updateCertificateProvider(
    target: RevisionedResourceRef,
    data: UpdateCertificateProviderDto
) {
    return request.putOperation(
        `${CERTIFICATE_PROVIDERS}/${target.id}`,
        data,
        withExpectedRevision(target.revision)
    );
}

export function verifyCertificateProvider(target: RevisionedResourceRef) {
    return request.postOperation(
        `${CERTIFICATE_PROVIDERS}/${target.id}/verify`,
        undefined,
        withExpectedRevision(target.revision)
    );
}

export function removeCertificateProvider(target: RevisionedResourceRef) {
    return request.deleteOperation(
        `${CERTIFICATE_PROVIDERS}/${target.id}`,
        withExpectedRevision(target.revision)
    );
}

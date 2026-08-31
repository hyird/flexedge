import { skipToken, useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { taskQueryKeys } from '@/pages/task/task.types';
import type { RevisionedResourceRef } from '@/utils/http';
import {
    createCertificate,
    downloadCertificate,
    getCertificate,
    getCertificates,
    removeCertificate,
    renewCertificate,
    saveCertificate,
} from './certificate.api';
import {
    type CertificateConfig,
    type CertificateQuery,
    certificateQueryKeys,
    type DownloadCertificateCommand,
} from './certificate.types';

export function useCertificateList(query: CertificateQuery, enabled = true) {
    return useQuery({
        queryKey: [...certificateQueryKeys.lists(), query],
        queryFn: () => getCertificates(query),
        enabled,
        refetchInterval: (state) =>
            state.state.data?.list.some((item) =>
                ['pending', 'issuing', 'renewing'].includes(item.status)
            )
                ? 3000
                : false,
    });
}

export function useCertificateDetail(id: string | undefined) {
    return useQuery({
        queryKey: certificateQueryKeys.detail(id ?? ''),
        queryFn: id ? () => getCertificate(id) : skipToken,
        refetchInterval: (state) =>
            state.state.data && ['pending', 'issuing', 'renewing'].includes(state.state.data.status)
                ? 3000
                : false,
    });
}

export function useCertificateCreate() {
    return useMutationWithMessage({
        mutationFn: createCertificate,
        successMessage: '证书申请已提交',
        invalidateKeys: [certificateQueryKeys.all, taskQueryKeys.all],
    });
}

export function useCertificateSave() {
    return useMutationWithMessage({
        mutationFn: ({
            target,
            config,
        }: {
            target: RevisionedResourceRef;
            config: CertificateConfig;
        }) => saveCertificate(target, config),
        successMessage: '自动续签设置已更新',
        invalidateKeys: [certificateQueryKeys.all, taskQueryKeys.all],
    });
}

export function useCertificateRenew() {
    return useMutationWithMessage({
        mutationFn: renewCertificate,
        successMessage: '重新签发已提交',
        invalidateKeys: [certificateQueryKeys.all, taskQueryKeys.all],
    });
}

export function useCertificateDelete() {
    return useMutationWithMessage({
        mutationFn: removeCertificate,
        successMessage: '证书已删除',
        invalidateKeys: [certificateQueryKeys.all, taskQueryKeys.all],
    });
}

export function useCertificateDownload() {
    return useMutationWithMessage({
        mutationFn: async ({ id, domain }: DownloadCertificateCommand) => {
            const blob = await downloadCertificate(id);
            const link = document.createElement('a');
            const url = URL.createObjectURL(blob);
            link.href = url;
            link.download = `${domain.startsWith('*.') ? domain.slice(2) : domain}.zip`;
            link.click();
            URL.revokeObjectURL(url);
        },
        successMessage: '证书已下载',
    });
}

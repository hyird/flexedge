import { useQuery } from '@tanstack/react-query';
import { useMutationWithMessage } from '@/hooks/useMutation';
import { taskQueryKeys } from '@/pages/task/task.types';
import {
    createCertificateProvider,
    getCertificateProviders,
    removeCertificateProvider,
    updateCertificateProvider,
    verifyCertificateProvider,
} from './certificate_provider.api';
import {
    certificateProviderQueryKeys,
    type SaveCertificateProviderCommand,
} from './certificate_provider.types';

export function useCertificateProviderList(enabled = true) {
    return useQuery({
        queryKey: certificateProviderQueryKeys.lists(),
        queryFn: getCertificateProviders,
        enabled,
    });
}

export function useCertificateProviderSave() {
    return useMutationWithMessage({
        mutationFn: (command: SaveCertificateProviderCommand) =>
            'id' in command && command.id !== undefined
                ? updateCertificateProvider(command, command.data)
                : createCertificateProvider(command.data),
        successMessage: (_, command) => (command.id ? '证书供应商已更新' : '证书供应商已添加'),
        invalidateKeys: [certificateProviderQueryKeys.all, taskQueryKeys.all],
    });
}

export function useCertificateProviderVerify() {
    return useMutationWithMessage({
        mutationFn: verifyCertificateProvider,
        successMessage: '证书供应商检测任务已提交',
        invalidateKeys: [certificateProviderQueryKeys.all, taskQueryKeys.all],
        invalidateOnError: true,
    });
}

export function useCertificateProviderDelete() {
    return useMutationWithMessage({
        mutationFn: removeCertificateProvider,
        successMessage: '证书供应商已删除',
        invalidateKeys: [certificateProviderQueryKeys.all, taskQueryKeys.all],
    });
}

/**
 * 通用 Mutation Hook
 */

import {
    type QueryKey,
    useQueryClient,
    useMutation as useReactMutation,
} from '@tanstack/react-query';
import { toast } from '@/components/ui/notification';

interface MutationOptions<TData, TVariables> {
    mutationFn: (variables: TVariables) => Promise<TData>;
    successMessage?: string | ((data: TData, variables: TVariables) => string);
    errorMessage?: string | ((error: Error) => string);
    invalidateKeys?: QueryKey[];
    invalidateOnError?: boolean;
    onSuccess?: (data: TData, variables: TVariables) => void;
    onError?: (error: Error, variables: TVariables) => void;
}

export function useMutationWithMessage<TData = void, TVariables = void>(
    options: MutationOptions<TData, TVariables>
) {
    const queryClient = useQueryClient();

    return useReactMutation({
        mutationFn: options.mutationFn,
        onSuccess: (data, variables) => {
            if (options.successMessage) {
                const msg =
                    typeof options.successMessage === 'function'
                        ? options.successMessage(data, variables)
                        : options.successMessage;
                toast.success(msg);
            }

            options.invalidateKeys?.forEach((key) => {
                queryClient.invalidateQueries({ queryKey: key });
            });

            options.onSuccess?.(data, variables);
        },
        onError: (error: Error, variables) => {
            if (options.errorMessage) {
                const msg =
                    typeof options.errorMessage === 'function'
                        ? options.errorMessage(error)
                        : options.errorMessage;
                toast.error(msg);
            }

            if (options.invalidateOnError) {
                options.invalidateKeys?.forEach((key) => {
                    queryClient.invalidateQueries({ queryKey: key });
                });
            }

            options.onError?.(error, variables);
        },
    });
}

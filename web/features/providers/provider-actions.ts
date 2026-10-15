import { useMutation, useQueryClient } from '@tanstack/react-query'
import { queryKeys } from '@/lib/query-keys'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { removeProvider, verifyProvider } from './data'
import type { ProviderTarget } from './types'

/** Verify and delete actions of both providers' edit sheets. `remove` is
 * awaited by `ConfirmDialog`, which shows failures inline. */
export function useProviderActions() {
  const client = useQueryClient()
  // Active list streams receive the change; inactive views refetch when shown.
  const refresh = (kind: ProviderTarget['kind']) =>
    client.invalidateQueries({
      queryKey: [...queryKeys.providers, kind],
      refetchType: 'none',
    })
  const verify = useMutation({
    mutationFn: verifyProvider,
    onSuccess: (response, target) => {
      toastWriteSuccess(response.message || '验证任务已提交', response.data)
      void refresh(target.kind)
    },
  })
  const remove = async (target: ProviderTarget) => {
    const response = await removeProvider(target)
    toastWriteSuccess(response.message || '已删除', response.data)
    void refresh(target.kind)
  }
  return {
    verify: (target: ProviderTarget) => verify.mutate(target),
    isVerifying: (id: string) =>
      verify.isPending && verify.variables?.id === id,
    remove,
  }
}

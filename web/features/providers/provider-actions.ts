import { useMutation, useQueryClient } from '@tanstack/react-query'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterProviderWrite, removeProvider, verifyProvider } from './data'
import type { ProviderTarget } from './types'

/** Shared provider list and edit actions. `remove` is
 * awaited by `ConfirmDialog`, which shows failures inline. */
export function useProviderActions() {
  const client = useQueryClient()
  const verify = useMutation({
    mutationFn: verifyProvider,
    onSuccess: (response, target) => {
      toastWriteSuccess(response.message || '验证任务已提交', {
        resourceType: 'provider',
        resourceId: target.id,
      })
      void afterProviderWrite(client, target.kind)
    },
  })
  const remove = async (target: ProviderTarget) => {
    const response = await removeProvider(target)
    toastWriteSuccess(response.message || '已删除')
    void afterProviderWrite(client, target.kind)
  }
  return {
    verify: (target: ProviderTarget) => verify.mutate(target),
    isVerifying: (id: string) =>
      verify.isPending && verify.variables?.id === id,
    remove,
  }
}

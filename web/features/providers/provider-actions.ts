import { useMutation, useQueryClient } from '@tanstack/react-query'
import { toast } from '@heroui/react'
import { queryKeys } from '@/lib/query-keys'
import { removeProvider, verifyProvider } from './data'
import type { ProviderTarget } from './types'

/** Row actions shared by both provider tabs. `remove` is awaited by
 * `ConfirmDialog`, which shows failures inline. */
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
      toast.success(response.message || '验证任务已提交')
      void refresh(target.kind)
    },
  })
  const remove = async (target: ProviderTarget) => {
    const response = await removeProvider(target)
    toast.success(response.message || '已删除')
    void refresh(target.kind)
  }
  return {
    verify: (target: ProviderTarget) => verify.mutate(target),
    isVerifying: (id: string) =>
      verify.isPending && verify.variables?.id === id,
    remove,
  }
}

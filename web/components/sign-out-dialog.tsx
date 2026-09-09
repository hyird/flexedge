import { useMutation, useQueryClient } from '@tanstack/react-query'
import { useNavigate } from '@tanstack/react-router'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { logout } from '@/features/auth/data'

type Props = {
  open: boolean
  onOpenChange: (open: boolean) => void
}

export function SignOutDialog({ open, onOpenChange }: Props) {
  const queryClient = useQueryClient()
  const navigate = useNavigate()
  const mutation = useMutation({
    mutationFn: () => logout(queryClient),
    onSuccess: async () => {
      onOpenChange(false)
      await navigate({
        to: '/sign-in',
        search: { redirect: undefined },
        replace: true,
      })
    },
  })

  return (
    <ConfirmDialog
      open={open}
      onOpenChange={onOpenChange}
      title='退出登录'
      desc='确定要退出 FlexEdge 控制台吗？'
      confirmText={mutation.isPending ? '正在退出…' : '退出登录'}
      cancelBtnText='取消'
      destructive
      isLoading={mutation.isPending}
      handleConfirm={() => mutation.mutate()}
      className='sm:max-w-sm'
    />
  )
}

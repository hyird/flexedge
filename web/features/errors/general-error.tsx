import { Button } from '@heroui/react'
import { EmptyState } from '@/components/page'

export function GeneralError({
  reset,
}: {
  error?: unknown
  reset?: () => void
}) {
  return (
    <main className='grid min-h-dvh place-items-center px-5'>
      <EmptyState
        title='页面暂时不可用'
        description='页面加载失败，请重试。'
        action={
          <Button
            size='sm'
            onPress={() => (reset ? reset() : location.reload())}
          >
            重新加载
          </Button>
        }
      />
    </main>
  )
}

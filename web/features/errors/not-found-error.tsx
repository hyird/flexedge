import { useNavigate } from '@tanstack/react-router'
import { Button } from '@heroui/react'
import { EmptyState } from '@/components/page'

export function NotFoundError() {
  const navigate = useNavigate()
  return (
    <main className='grid min-h-dvh place-items-center px-5'>
      <EmptyState
        title='页面不存在'
        description='请检查地址，或返回工作台。'
        action={
          <Button size='sm' onPress={() => void navigate({ to: '/' })}>
            返回概览
          </Button>
        }
      />
    </main>
  )
}

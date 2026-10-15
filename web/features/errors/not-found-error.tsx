import { useNavigate, useRouter } from '@tanstack/react-router'
import { Button } from '@heroui/react'
import { ErrorLayout } from './error-layout'

export function NotFoundError() {
  const navigate = useNavigate()
  const router = useRouter()
  return (
    <ErrorLayout
      code='404'
      title='页面不存在'
      description='地址可能输入有误，或资源已被删除。可以返回概览，或使用 Ctrl K 搜索页面和资源。'
      actions={
        <>
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => void navigate({ to: '/' })}
          >
            返回概览
          </Button>
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => router.history.back()}
          >
            返回上一页
          </Button>
        </>
      }
    />
  )
}

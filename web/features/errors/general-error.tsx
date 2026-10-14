import { useNavigate } from '@tanstack/react-router'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { ErrorLayout } from './error-layout'

export function GeneralError({
  error,
  reset,
}: {
  error?: unknown
  reset?: () => void
}) {
  const navigate = useNavigate()
  return (
    <ErrorLayout
      code='页面错误'
      title='页面暂时不可用'
      description='页面加载时出现异常。可以重新加载；如果问题持续，请记录下方信息并联系管理员。'
      details={
        error !== undefined && (
          <pre className='w-full overflow-x-auto rounded-lg border border-border bg-surface px-3 py-2 font-mono text-xs leading-5 whitespace-pre-wrap text-muted'>
            {apiErrorMessage(error)}
          </pre>
        )
      }
      actions={
        <>
          <Button
            size='sm'
            onPress={() => (reset ? reset() : location.reload())}
          >
            重新加载
          </Button>
          <Button
            size='sm'
            variant='tertiary'
            onPress={() => void navigate({ to: '/' })}
          >
            返回概览
          </Button>
        </>
      }
    />
  )
}

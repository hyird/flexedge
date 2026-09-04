import { isNotFound } from '@tanstack/react-router'
import { ServerCrash } from 'lucide-react'
import { Button } from '@/components/ui/button'

export function GeneralError({ error }: { error: unknown }) {
  if (isNotFound(error)) return <NotFoundFallback />

  return (
    <div className='grid min-h-svh place-items-center px-6 text-center'>
      <div className='max-w-md space-y-4'>
        <ServerCrash className='mx-auto size-10 text-muted-foreground' />
        <h1 className='text-2xl font-bold tracking-tight'>页面暂时不可用</h1>
        <p className='text-sm text-muted-foreground'>
          页面加载时发生错误，请刷新后重试。
        </p>
        <Button onClick={() => window.location.reload()}>刷新页面</Button>
      </div>
    </div>
  )
}

function NotFoundFallback() {
  return (
    <div className='grid min-h-svh place-items-center px-6 text-center'>
      <div>
        <div className='text-7xl font-bold tracking-tighter'>404</div>
        <p className='mt-3 text-muted-foreground'>没有找到这个页面。</p>
      </div>
    </div>
  )
}

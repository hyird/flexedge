import { Link } from '@tanstack/react-router'
import { Button } from '@/components/ui/button'

export function NotFoundError() {
  return (
    <div className='grid min-h-svh place-items-center px-6 text-center'>
      <div className='space-y-4'>
        <div className='text-7xl font-bold tracking-tighter'>404</div>
        <div>
          <h1 className='text-xl font-semibold'>页面不存在</h1>
          <p className='mt-1 text-sm text-muted-foreground'>
            地址可能已变更，或者你没有访问权限。
          </p>
        </div>
        <Button asChild>
          <Link to='/'>返回概览</Link>
        </Button>
      </div>
    </div>
  )
}

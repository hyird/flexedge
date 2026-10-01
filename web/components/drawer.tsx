import { type ReactNode } from 'react'
import { Sheet } from '@heroui-pro/react'
import { cn } from '@/lib/utils'

export function Drawer({
  title,
  children,
  onClose,
  size = 'md',
}: {
  title: string
  children: ReactNode
  onClose: () => void
  size?: 'md' | 'lg'
}) {
  return (
    <Sheet
      isOpen
      placement='right'
      isHandleOnly
      shouldAutoFocus
      onOpenChange={(open) => {
        if (!open) onClose()
      }}
    >
      <Sheet.Backdrop>
        <Sheet.Content
          className={cn(
            'w-full max-w-full',
            size === 'lg' ? 'sm:max-w-5xl' : 'sm:max-w-2xl'
          )}
        >
          <Sheet.Dialog>
            <Sheet.CloseTrigger aria-label='关闭日志抽屉' />
            <Sheet.Header className='pr-14'>
              <Sheet.Heading>{title}</Sheet.Heading>
            </Sheet.Header>
            <Sheet.Body className='min-h-0 pb-5'>{children}</Sheet.Body>
          </Sheet.Dialog>
        </Sheet.Content>
      </Sheet.Backdrop>
    </Sheet>
  )
}

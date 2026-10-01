import { type ReactNode } from 'react'
import { Sheet } from '@heroui-pro/react'
import { cn } from '@/lib/utils'

export function Drawer({
  title,
  children,
  onClose,
  busy = false,
  size = 'md',
}: {
  title: string
  children: ReactNode
  onClose: () => void
  busy?: boolean
  size?: 'sm' | 'md' | 'lg'
}) {
  return (
    <Sheet
      isOpen
      placement='right'
      isHandleOnly
      shouldAutoFocus
      isDismissable={!busy}
      onOpenChange={(open) => {
        if (!open && !busy) onClose()
      }}
    >
      <Sheet.Backdrop isKeyboardDismissDisabled={busy}>
        <Sheet.Content
          className={cn(
            'w-full max-w-full',
            size === 'lg'
              ? 'sm:max-w-5xl'
              : size === 'sm'
                ? 'sm:max-w-lg'
                : 'sm:max-w-2xl'
          )}
        >
          <Sheet.Dialog>
            <Sheet.CloseTrigger aria-label='关闭抽屉' isDisabled={busy} />
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

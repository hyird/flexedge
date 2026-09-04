import { cn } from '@/lib/utils'
import { Button } from '@/components/ui/button'
import {
  Sheet,
  SheetContent,
  SheetDescription,
  SheetFooter,
  SheetHeader,
  SheetTitle,
} from '@/components/ui/sheet'

type ConfirmDialogProps = {
  open: boolean
  onOpenChange: (open: boolean) => void
  title: React.ReactNode
  disabled?: boolean
  desc: React.JSX.Element | string
  cancelBtnText?: string
  confirmText?: React.ReactNode
  destructive?: boolean
  isLoading?: boolean
  className?: string
  children?: React.ReactNode
} & (
  | { form: string; handleConfirm?: undefined }
  | { form?: undefined; handleConfirm: () => void }
)

export function ConfirmDialog(props: ConfirmDialogProps) {
  const {
    title,
    desc,
    children,
    className,
    confirmText,
    cancelBtnText,
    destructive,
    isLoading,
    disabled = false,
    form,
    handleConfirm,
    ...actions
  } = props
  return (
    <Sheet {...actions}>
      <SheetContent className={cn('sm:max-w-md', className)}>
        <SheetHeader>
          <SheetTitle>{title}</SheetTitle>
          <SheetDescription asChild>
            <div>{desc}</div>
          </SheetDescription>
        </SheetHeader>
        {children && <div className='px-4'>{children}</div>}
        <SheetFooter>
          <Button
            type='button'
            variant='outline'
            disabled={isLoading}
            onClick={() => actions.onOpenChange(false)}
          >
            {cancelBtnText ?? '取消'}
          </Button>
          <Button
            type={form ? 'submit' : 'button'}
            form={form}
            onClick={handleConfirm}
            variant={destructive ? 'destructive' : 'default'}
            disabled={disabled || isLoading}
          >
            {confirmText ?? '继续'}
          </Button>
        </SheetFooter>
      </SheetContent>
    </Sheet>
  )
}

import { type FormEvent, type ReactNode } from 'react'
import { Sheet } from '@heroui-pro/react'
import { Alert, Button } from '@heroui/react'
import { cn } from '@/lib/utils'

const sheetWidths = {
  sm: 'sm:max-w-md',
  md: 'sm:max-w-xl',
  lg: 'sm:max-w-3xl',
} as const

/** Right-side sheet for creation and small edits. The body is a `<form>`:
 * Enter submits, the footer holds Cancel + submit, and while `isSubmitting`
 * the sheet cannot be dismissed. Submission errors render inside the form.
 * Read-only sheets (no `onSubmit`) have no footer: the close button in the
 * corner is their only exit. */
export function FormSheet({
  isOpen,
  onClose,
  title,
  description,
  size = 'md',
  children,
  onSubmit,
  submitLabel = '保存',
  submitTone = 'primary',
  isSubmitting = false,
  isSubmitDisabled = false,
  error,
}: {
  isOpen: boolean
  onClose: () => void
  title: string
  description?: ReactNode
  size?: keyof typeof sheetWidths
  children: ReactNode
  /** Receives the submit event; pass react-hook-form's `handleSubmit(fn)`
   * directly or call `event.preventDefault()` yourself. Omit for read-only sheets. */
  onSubmit?: (event: FormEvent<HTMLFormElement>) => unknown
  submitLabel?: string
  submitTone?: 'primary' | 'danger'
  isSubmitting?: boolean
  isSubmitDisabled?: boolean
  /** Form-level error (e.g. `apiErrorMessage(mutation.error)`). */
  error?: ReactNode
}) {
  return (
    <Sheet
      isOpen={isOpen}
      placement='right'
      isHandleOnly
      shouldAutoFocus
      isDismissable={!isSubmitting}
      onOpenChange={(open) => {
        if (!open && !isSubmitting) onClose()
      }}
    >
      <Sheet.Backdrop isKeyboardDismissDisabled={isSubmitting}>
        <Sheet.Content className={cn('w-full max-w-full', sheetWidths[size])}>
          <Sheet.Dialog>
            <form
              noValidate
              className='flex min-h-0 flex-1 flex-col'
              onSubmit={(event) => {
                if (!onSubmit) return event.preventDefault()
                void onSubmit(event)
              }}
            >
              <Sheet.CloseTrigger aria-label='关闭' isDisabled={isSubmitting} />
              <Sheet.Header className='gap-1 pr-14'>
                <Sheet.Heading>{title}</Sheet.Heading>
                {description && (
                  <p className='text-sm leading-6 text-muted'>{description}</p>
                )}
              </Sheet.Header>
              {/* `relative`: absolutely positioned descendants (sr-only text)
                  must lay out inside the scrolling body; otherwise they
                  overflow the clipped dialog, which scrollIntoView then
                  scrolls, moving the header and footer. The scrollbar keeps
                  its space while the content fits, so content that grows or
                  shrinks (tabs, validation errors) never shifts sideways. */}
              <Sheet.Body className='relative flex min-h-0 [scrollbar-gutter:stable] flex-col gap-4 pb-6'>
                {error && (
                  <Alert status='danger'>
                    <Alert.Indicator />
                    <Alert.Content>
                      <Alert.Description>{error}</Alert.Description>
                    </Alert.Content>
                  </Alert>
                )}
                {children}
              </Sheet.Body>
              {onSubmit && (
                <Sheet.Footer className='justify-end gap-2 border-t border-border'>
                  <Button
                    size='sm'
                    variant='tertiary'
                    isDisabled={isSubmitting}
                    onPress={onClose}
                  >
                    取消
                  </Button>
                  <Button
                    size='sm'
                    type='submit'
                    variant={submitTone}
                    isPending={isSubmitting}
                    isDisabled={isSubmitDisabled}
                  >
                    {submitLabel}
                  </Button>
                </Sheet.Footer>
              )}
            </form>
          </Sheet.Dialog>
        </Sheet.Content>
      </Sheet.Backdrop>
    </Sheet>
  )
}

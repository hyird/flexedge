import { type ReactNode, useState } from 'react'
import { Alert, AlertDialog, Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'

type ConfirmDialogProps = {
  isOpen: boolean
  onClose: () => void
  /** Names the object: `删除网站 qa.example.test？` */
  title: string
  /** States the consequence. */
  description: ReactNode
  /** Names the action: `删除网站`. */
  confirmLabel: string
  /** `danger` for destructive actions, `accent` for reversible confirmations. */
  tone?: 'danger' | 'accent'
  /** May be async; the dialog stays open and locked while it runs, closes on
   * success and shows the error inline on failure. */
  onConfirm: () => unknown
}

/** Centered confirmation for dangerous or irreversible operations. */
export function ConfirmDialog({
  isOpen,
  onClose,
  ...content
}: ConfirmDialogProps) {
  const [isPending, setPending] = useState(false)
  return (
    <AlertDialog.Backdrop
      isOpen={isOpen}
      onOpenChange={(open) => {
        if (!open && !isPending) onClose()
      }}
      isDismissable={!isPending}
      isKeyboardDismissDisabled={isPending}
    >
      <AlertDialog.Container size='sm'>
        <AlertDialog.Dialog>
          <ConfirmDialogContent
            {...content}
            onClose={onClose}
            isPending={isPending}
            setPending={setPending}
          />
        </AlertDialog.Dialog>
      </AlertDialog.Container>
    </AlertDialog.Backdrop>
  )
}

// Mounted with the dialog so each opening starts without a stale error.
function ConfirmDialogContent({
  title,
  description,
  confirmLabel,
  tone = 'danger',
  onConfirm,
  onClose,
  isPending,
  setPending,
}: Omit<ConfirmDialogProps, 'isOpen'> & {
  isPending: boolean
  setPending: (pending: boolean) => void
}) {
  const [error, setError] = useState('')
  async function confirm() {
    if (isPending) return
    setPending(true)
    setError('')
    try {
      await onConfirm()
      setPending(false)
      onClose()
    } catch (reason) {
      setPending(false)
      setError(apiErrorMessage(reason))
    }
  }
  return (
    <>
      <AlertDialog.Header>
        <AlertDialog.Icon status={tone} />
        <AlertDialog.Heading>{title}</AlertDialog.Heading>
      </AlertDialog.Header>
      {/* Contains absolutely positioned descendants (see FormSheet). */}
      <AlertDialog.Body className='relative flex flex-col gap-3 text-sm leading-6 text-muted'>
        <div>{description}</div>
        {error && (
          <Alert status='danger'>
            <Alert.Indicator />
            <Alert.Content>
              <Alert.Description>{error}</Alert.Description>
            </Alert.Content>
          </Alert>
        )}
      </AlertDialog.Body>
      <AlertDialog.Footer>
        <Button
          size='sm'
          variant='tertiary'
          isDisabled={isPending}
          onPress={onClose}
        >
          取消
        </Button>
        <Button
          size='sm'
          variant={tone === 'danger' ? 'danger' : 'primary'}
          isPending={isPending}
          onPress={() => void confirm()}
        >
          {confirmLabel}
        </Button>
      </AlertDialog.Footer>
    </>
  )
}

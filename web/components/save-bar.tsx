import { type ReactNode } from 'react'
import { CircleExclamation } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import {
  type UnsavedChangesGuard,
  useUnsavedChangesGuard,
} from '@/lib/unsaved-changes'
import { ConfirmDialog } from './confirm-dialog'

/** Confirmation shown while a guarded navigation is pending. */
export function UnsavedChangesDialog({
  guard,
}: {
  guard: UnsavedChangesGuard
}) {
  return (
    <ConfirmDialog
      isOpen={guard.isBlocked}
      onClose={guard.stay}
      title='放弃未保存的更改？'
      description='离开后，本页尚未保存的修改将丢失。'
      confirmLabel='放弃并离开'
      onConfirm={guard.leave}
    />
  )
}

/** Sticky bottom bar for in-page editors (website/node settings, DNS
 * records). Appears only while dirty and guards navigation. */
export function SaveBar({
  isDirty,
  isSaving,
  onSave,
  onDiscard,
  saveLabel = '保存',
  message = '有未保存的更改',
  error,
}: {
  isDirty: boolean
  isSaving: boolean
  onSave: () => unknown
  onDiscard: () => void
  saveLabel?: string
  message?: string
  /** Validation or request failure next to the message. */
  error?: ReactNode
}) {
  const guard = useUnsavedChangesGuard(isDirty && !isSaving)
  return (
    <>
      {isDirty && (
        <div
          role='region'
          aria-label='保存更改'
          className='sticky bottom-0 z-20 -mx-4 mt-auto -mb-10 border-t border-border bg-surface/95 px-4 py-3 backdrop-blur sm:-mx-6 sm:px-6'
        >
          <div className='flex flex-wrap items-center justify-between gap-3'>
            <p className='flex min-w-0 items-center gap-2 text-sm'>
              <CircleExclamation
                aria-hidden
                className='size-4 shrink-0 text-warning'
              />
              <span className='font-medium'>{message}</span>
              {error && <span className='text-danger'>{error}</span>}
            </p>
            <div className='flex items-center gap-2'>
              <Button
                size='sm'
                variant='tertiary'
                isDisabled={isSaving}
                onPress={onDiscard}
              >
                放弃
              </Button>
              <Button
                size='sm'
                isPending={isSaving}
                onPress={() => void onSave()}
              >
                {saveLabel}
              </Button>
            </div>
          </div>
        </div>
      )}
      <UnsavedChangesDialog guard={guard} />
    </>
  )
}

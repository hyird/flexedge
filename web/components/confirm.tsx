import { useState } from 'react'
import { Button } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Dialog, Notice } from './forms'

export function Confirm({
  title,
  description,
  onConfirm,
  onClose,
  busy = false,
}: {
  title: string
  description: string
  onConfirm: () => unknown | Promise<unknown>
  onClose: () => void
  busy?: boolean
}) {
  const [pending, setPending] = useState(false)
  const [error, setError] = useState('')
  const working = busy || pending
  async function submit() {
    if (working) return
    setPending(true)
    setError('')
    try {
      await onConfirm()
      onClose()
    } catch (reason) {
      setError(apiErrorMessage(reason))
    } finally {
      setPending(false)
    }
  }
  return (
    <Dialog title={title} onClose={onClose} busy={working}>
      <div className='flex flex-col gap-4'>
        <p className='text-sm leading-6'>{description}</p>
        <Notice>{error}</Notice>
        <div className='flex justify-end gap-2'>
          <Button
            size='sm'
            variant='tertiary'
            onPress={onClose}
            isDisabled={working}
          >
            取消
          </Button>
          <Button
            size='sm'
            variant='danger-soft'
            isPending={working}
            onPress={() => void submit()}
          >
            确认
          </Button>
        </div>
      </div>
    </Dialog>
  )
}

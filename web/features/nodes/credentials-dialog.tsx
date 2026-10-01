import { useState } from 'react'
import { Button, toast } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { Dialog, Notice } from '@/components/forms'
import type { NodeCredentials } from './data'

export function CredentialsDialog({
  credentials,
  onClose,
}: {
  credentials: NodeCredentials
  onClose: () => void
}) {
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  async function copy() {
    setBusy(true)
    setError('')
    try {
      await navigator.clipboard.writeText(
        `NODE_ID=${credentials.node_id}\nNODE_SECRET=${credentials.secret}`
      )
      toast.success('接入凭据已复制')
    } catch (error) {
      setError(apiErrorMessage(error))
    } finally {
      setBusy(false)
    }
  }
  return (
    <Dialog title='节点接入凭据' onClose={onClose}>
      <div className='grid gap-4'>
        <p className='text-sm text-muted'>在节点启动配置中填写以下凭据。</p>
        <dl className='grid gap-3'>
          <div>
            <dt className='mb-1 text-xs text-muted'>Node ID</dt>
            <dd className='rounded-lg bg-surface-secondary p-3 font-mono text-xs break-all'>
              {credentials.node_id}
            </dd>
          </div>
          <div>
            <dt className='mb-1 text-xs text-muted'>Secret</dt>
            <dd className='rounded-lg bg-surface-secondary p-3 font-mono text-xs break-all'>
              {credentials.secret}
            </dd>
          </div>
        </dl>
        {error && <Notice>{error}</Notice>}
        <div className='flex justify-end gap-2'>
          <Button
            size='sm'
            variant='secondary'
            isPending={busy}
            onPress={() => void copy()}
          >
            复制凭据
          </Button>
          <Button size='sm' onPress={onClose}>
            完成
          </Button>
        </div>
      </div>
    </Dialog>
  )
}

import { useState } from 'react'
import { TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { useProviderActions } from './provider-actions'
import type { CertificateProvider, DnsProvider, ProviderTarget } from './types'

type ProviderRecord = DnsProvider | CertificateProvider

/** Danger button at the end of an edit sheet; `onRemoved` closes the sheet. */
export function ProviderRemoval({
  kind,
  provider,
  label,
  title,
  description,
  isDisabled,
  onRemoved,
}: {
  kind: ProviderTarget['kind']
  provider: ProviderRecord
  /** Button and confirm label: `删除账号`. */
  label: string
  title: string
  description: string
  isDisabled: boolean
  onRemoved: () => void
}) {
  const actions = useProviderActions()
  const [removing, setRemoving] = useState(false)
  return (
    <>
      <Button
        size='sm'
        variant='danger-soft'
        className='self-start'
        isDisabled={isDisabled}
        onPress={() => setRemoving(true)}
      >
        <TrashBin />
        {label}
      </Button>
      <ConfirmDialog
        isOpen={removing}
        onClose={() => setRemoving(false)}
        title={title}
        description={description}
        confirmLabel={label}
        onConfirm={async () => {
          await actions.remove({
            kind,
            id: provider.id,
            revision: provider.revision,
          })
          onRemoved()
        }}
      />
    </>
  )
}

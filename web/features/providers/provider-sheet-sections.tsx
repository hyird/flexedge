import { useState } from 'react'
import { TrashBin } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
import { describeStatus } from '@/lib/status'
import { ConfirmDialog } from '@/components/confirm-dialog'
import { Notice } from '@/components/forms'
import { RelativeTime } from '@/components/relative-time'
import { DescriptionList } from '@/components/section'
import { StatusChip } from '@/components/status-chip'
import { useProviderActions } from './provider-actions'
import { providerStatuses, providerStatusReserve } from './provider-display'
import type { CertificateProvider, DnsProvider, ProviderTarget } from './types'

type ProviderRecord = DnsProvider | CertificateProvider

/** `验证` block of an edit sheet. Verification checks the saved credential,
 * so it is disabled while the form has unsaved changes. */
export function ProviderVerification({
  kind,
  provider,
  isDirty,
}: {
  kind: ProviderTarget['kind']
  provider: ProviderRecord
  isDirty: boolean
}) {
  const actions = useProviderActions()
  return (
    <div className='flex flex-col gap-3 border-t border-border pt-4'>
      <div className='flex items-center justify-between gap-2'>
        <h3 className='subsection-title'>验证</h3>
        <Button
          size='sm'
          variant='tertiary'
          isPending={actions.isVerifying(provider.id)}
          isDisabled={isDirty}
          onPress={() =>
            actions.verify({
              kind,
              id: provider.id,
              revision: provider.revision,
            })
          }
        >
          验证凭据
        </Button>
      </div>
      {isDirty && <p className='text-sm text-muted'>先保存再验证</p>}
      <DescriptionList
        items={[
          {
            label: '状态',
            value: (
              <StatusChip
                {...describeStatus(providerStatuses, provider.status)}
                reserve={providerStatusReserve}
              />
            ),
          },
          {
            label: '最近验证',
            value: <RelativeTime value={provider.last_verified_at} />,
          },
        ]}
      />
      {provider.last_error && (
        <Notice>最近一次验证失败：{provider.last_error}</Notice>
      )}
    </div>
  )
}

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

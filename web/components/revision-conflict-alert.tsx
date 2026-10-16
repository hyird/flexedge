import { Alert, Button } from '@heroui/react'
import type { RevisionDraft } from '@/lib/revision-draft'

/** Notice of a `useRevisionDraft` editor whose resource changed elsewhere:
 * a warning while the dirty draft is stale, an error once saving it lost the
 * revision race. Reloading loads the latest snapshot and drops the draft. */
export function RevisionConflictAlert({
  draft,
  subject = '配置',
}: {
  draft: Pick<
    RevisionDraft<{ revision: number }, unknown>,
    'conflict' | 'stale' | 'reload' | 'isReloading'
  >
  /** What the draft edits, e.g. `配置` or `记录`. */
  subject?: string
}) {
  if (!draft.conflict && !draft.stale) return null
  return (
    <Alert status={draft.conflict ? 'danger' : 'warning'}>
      <Alert.Indicator />
      <Alert.Content>
        <Alert.Title>
          {draft.conflict
            ? `保存失败，${subject}已被其他人更新`
            : `${subject}已被其他人更新`}
        </Alert.Title>
        <Alert.Description>
          重新加载会载入最新{subject}并丢弃未保存的更改。
        </Alert.Description>
      </Alert.Content>
      <Button
        size='sm'
        variant='tertiary'
        className='shrink-0 self-center'
        isPending={draft.isReloading}
        onPress={() => void draft.reload()}
      >
        重新加载
      </Button>
    </Alert>
  )
}

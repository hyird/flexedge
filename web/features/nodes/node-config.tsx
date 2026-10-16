import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Skeleton } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { useDraftForm, useRevisionDraft } from '@/lib/revision-draft'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { SaveBar } from '@/components/save-bar'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { afterNodeWrite, nodeQuery, updateNode } from './data'
import { NodeConfigFields } from './node-config-fields'
import { NodeCredentialActions } from './node-credential-actions'
import {
  nodeFormSchema,
  nodeFormValues,
  nodeInputFromForm,
  type NodeFormValues,
} from './node-form'
import type { Node } from './types'

const route = getRouteApi('/_authenticated/nodes/$nodeId')

function NodeConfigEditor({
  node,
  reload,
}: {
  node: Node
  reload: () => Promise<Node | undefined>
}) {
  const client = useQueryClient()
  // Runtime patches keep the revision, so only configuration changes (ours
  // or someone else's) move the server ahead of a dirty draft.
  const draft = useRevisionDraft({
    server: node,
    save: (values: NodeFormValues, revision) =>
      updateNode(
        { id: node.id, revision: revision ?? node.revision },
        nodeInputFromForm(values)
      ),
    // The page's node stream pushes the saved configuration (header included).
    onSaved: (response, values) => {
      // Only registered nodes have a configuration task.
      toastWriteSuccess(
        response.message,
        node.runtime.registration_status === 'pending'
          ? undefined
          : { resourceType: 'node', resourceId: node.id }
      )
      form.reset(values)
      afterNodeWrite(client)
    },
    load: (latest) => form.reset(nodeFormValues(latest ?? node)),
    reload,
  })
  const form = useForm<NodeFormValues>({
    resolver: zodResolver(nodeFormSchema),
    values: nodeFormValues(draft.base ?? node),
  })
  useDraftForm(form, draft.track)
  const { isDirty } = form.formState
  const save = form.handleSubmit(draft.save)
  const hasErrors = Object.keys(form.formState.errors).length > 0
  return (
    <div className='flex flex-1 flex-col gap-6'>
      <RevisionConflictAlert draft={draft} />
      <form
        noValidate
        aria-label='节点配置'
        className='flex flex-col gap-6'
        onSubmit={save}
      >
        <NodeConfigFields
          form={form}
          variant='page'
          disabled={draft.isSaving}
        />
      </form>
      <NodeCredentialActions node={node} />
      <SaveBar
        isDirty={isDirty}
        isSaving={draft.isSaving}
        onDiscard={draft.discard}
        onSave={save}
        saveLabel='保存并分发'
        error={
          draft.conflict
            ? '数据已被其他人更新'
            : draft.error
              ? apiErrorMessage(draft.error)
              : hasErrors
                ? '请检查标红的字段'
                : undefined
        }
      />
    </div>
  )
}

/** Node detail config tab: in-page editor with a sticky save bar and a
 * navigation guard while there are unsaved changes and the `接入凭据` card
 * (reveal / reset). */
export function NodeConfig() {
  const { nodeId } = route.useParams()
  const query = useQuery(nodeQuery(nodeId))
  if (!query.data)
    return (
      <div role='status' aria-label='正在加载节点配置' className='grid gap-6'>
        <Skeleton className='h-40 rounded-2xl' />
        <Skeleton className='h-48 rounded-2xl' />
      </div>
    )
  return (
    <NodeConfigEditor
      key={query.data.id}
      node={query.data}
      reload={async () => (await query.refetch()).data}
    />
  )
}

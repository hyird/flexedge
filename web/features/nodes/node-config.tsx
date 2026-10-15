import { useEffect, useState } from 'react'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Alert, Button, Skeleton, toast } from '@heroui/react'
import { apiErrorMessage, isRevisionConflict } from '@/lib/api'
import { SaveBar } from '@/components/save-bar'
import { nodeQuery, updateNode } from './data'
import { NodeConfigFields } from './node-config-fields'
import { NodeCredentialActions } from './node-credential-actions'
import {
  nodeFormSchema,
  nodeFormValues,
  nodeInputFromForm,
  type NodeFormValues,
} from './node-form'
import { NodeLifecycleActions } from './node-lifecycle-actions'
import type { Node } from './types'

const route = getRouteApi('/_authenticated/nodes/$nodeId')

function NodeConfigEditor({
  node,
  reload,
}: {
  node: Node
  reload: () => Promise<Node | undefined>
}) {
  const form = useForm<NodeFormValues>({
    resolver: zodResolver(nodeFormSchema),
    defaultValues: nodeFormValues(node),
  })
  // The draft's base revision. Runtime patches keep the revision, so only
  // configuration changes (ours or someone else's) move the server ahead.
  const [baseRevision, setBaseRevision] = useState(node.revision)
  const [server, setServer] = useState(node)
  const { isDirty } = form.formState
  if (server.revision !== node.revision) {
    setServer(node)
    // A clean draft follows the server; a dirty one keeps its base so the
    // stale-draft warning and the If-Match conflict can surface.
    if (!isDirty) setBaseRevision(node.revision)
  }
  useEffect(() => {
    if (!form.formState.isDirty) form.reset(nodeFormValues(server))
  }, [server, form])
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (values: NodeFormValues) =>
      updateNode(
        { id: node.id, revision: baseRevision },
        nodeInputFromForm(values)
      ),
    // The page's node stream pushes the saved configuration (header included).
    onSuccess: (response, values) => {
      toast.success(response.message)
      form.reset(values)
    },
  })
  const discard = () => {
    form.reset(nodeFormValues(server))
    setBaseRevision(server.revision)
    mutation.reset()
  }
  const reloadLatest = async () => {
    const latest = (await reload()) ?? server
    form.reset(nodeFormValues(latest))
    setBaseRevision(latest.revision)
    mutation.reset()
  }
  const conflict = mutation.isError && isRevisionConflict(mutation.error)
  const stale = isDirty && server.revision !== baseRevision
  const hasErrors = Object.keys(form.formState.errors).length > 0
  return (
    <div className='flex flex-1 flex-col gap-6'>
      {(conflict || stale) && (
        <Alert status='warning'>
          <Alert.Indicator />
          <Alert.Content>
            <Alert.Title>配置已更新</Alert.Title>
            <Alert.Description>
              此配置已被其他人更新，重新加载会载入最新配置并丢弃本页未保存的更改。
            </Alert.Description>
          </Alert.Content>
          <Button
            size='sm'
            variant='tertiary'
            className='shrink-0 self-center'
            onPress={() => void reloadLatest()}
          >
            重新加载
          </Button>
        </Alert>
      )}
      <form
        noValidate
        aria-label='节点配置'
        className='flex flex-col gap-6'
        onSubmit={form.handleSubmit((values) => mutation.mutate(values))}
      >
        <NodeConfigFields
          form={form}
          variant='page'
          disabled={mutation.isPending}
          basicFooter={<NodeLifecycleActions node={node} />}
        />
      </form>
      <NodeCredentialActions node={node} />
      <SaveBar
        isDirty={isDirty}
        isSaving={mutation.isPending}
        onDiscard={discard}
        onSave={form.handleSubmit((values) => mutation.mutate(values))}
        saveLabel='保存并分发'
        error={
          conflict
            ? '数据已被其他人更新'
            : mutation.isError
              ? apiErrorMessage(mutation.error)
              : hasErrors
                ? '请检查标红的字段'
                : undefined
        }
      />
    </div>
  )
}

/** Node detail config tab: in-page editor with a sticky save bar and a
 * navigation guard while there are unsaved changes, `删除节点` under the basic
 * fields and the `接入凭据` card (reveal / reset). */
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

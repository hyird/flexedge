import { useEffect, useState } from 'react'
import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Alert, Button, Skeleton, toast } from '@heroui/react'
import { apiErrorMessage, isRevisionConflict } from '@/lib/api'
import { SaveBar } from '@/components/save-bar'
import { Section } from '@/components/section'
import { ClusterAccessFields, ClusterBasicFields } from './cluster-fields'
import {
  clusterFormSchema,
  clusterFormValues,
  type ClusterFormValues,
} from './cluster-form'
import { ClusterLifecycleActions } from './cluster-lifecycle-actions'
import { clusterSnapshotKey, clusterSnapshotQuery, saveCluster } from './data'
import type { Cluster } from './types'

const route = getRouteApi('/_authenticated/clusters/$clusterId')

function ClusterConfigEditor({
  cluster,
  reload,
}: {
  cluster: Cluster
  reload: () => Promise<Cluster | undefined>
}) {
  const client = useQueryClient()
  const form = useForm<ClusterFormValues>({
    resolver: zodResolver(clusterFormSchema),
    defaultValues: clusterFormValues(cluster),
  })
  // The draft's base revision; the snapshot moves ahead on re-reads.
  const [baseRevision, setBaseRevision] = useState(cluster.revision)
  const [server, setServer] = useState(cluster)
  const { isDirty } = form.formState
  if (server.revision !== cluster.revision) {
    setServer(cluster)
    // A clean draft follows the server; a dirty one keeps its base so the
    // stale-draft warning and the If-Match conflict can surface.
    if (!isDirty) setBaseRevision(cluster.revision)
  }
  useEffect(() => {
    if (!form.formState.isDirty) form.reset(clusterFormValues(server))
  }, [server, form])
  const mutation = useMutation({
    meta: { inlineError: true },
    mutationFn: (values: ClusterFormValues) =>
      saveCluster(values, { id: cluster.id, revision: baseRevision }),
    onSuccess: (response, values) => {
      toast.success(response.message)
      form.reset(values)
      void client.invalidateQueries({ queryKey: clusterSnapshotKey })
    },
  })
  const save = form.handleSubmit((values) => mutation.mutate(values))
  const discard = () => {
    form.reset(clusterFormValues(server))
    setBaseRevision(server.revision)
    mutation.reset()
  }
  const reloadLatest = async () => {
    const latest = (await reload()) ?? server
    form.reset(clusterFormValues(latest))
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
        aria-label='集群配置'
        className='flex flex-col gap-6'
        onSubmit={save}
      >
        <Section title='基本信息'>
          <div className='flex flex-col gap-4'>
            <ClusterBasicFields form={form} disabled={mutation.isPending} />
            <ClusterLifecycleActions cluster={server} />
          </div>
        </Section>
        <Section title='接入域名'>
          <div className='flex flex-col gap-4'>
            <ClusterAccessFields
              form={form}
              disabled={mutation.isPending}
              cluster={server}
            />
          </div>
        </Section>
      </form>
      <SaveBar
        isDirty={isDirty}
        isSaving={mutation.isPending}
        onDiscard={discard}
        onSave={save}
        saveLabel='保存'
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

/** Cluster detail 配置 tab: in-page editor of the header's snapshot with a
 * sticky save bar and a navigation guard while there are unsaved changes. */
export function ClusterConfig() {
  const { clusterId } = route.useParams()
  const query = useQuery(clusterSnapshotQuery(clusterId))
  if (!query.data)
    return (
      <div role='status' aria-label='正在加载集群配置' className='grid gap-6'>
        <Skeleton className='h-40 rounded-2xl' />
        <Skeleton className='h-48 rounded-2xl' />
      </div>
    )
  return (
    <ClusterConfigEditor
      key={query.data.id}
      cluster={query.data}
      reload={async () => (await query.refetch()).data}
    />
  )
}

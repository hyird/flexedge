import { useForm } from 'react-hook-form'
import { zodResolver } from '@hookform/resolvers/zod'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { getRouteApi } from '@tanstack/react-router'
import { Skeleton } from '@heroui/react'
import { apiErrorMessage } from '@/lib/api'
import { useDraftForm, useRevisionDraft } from '@/lib/revision-draft'
import { RevisionConflictAlert } from '@/components/revision-conflict-alert'
import { SaveBar } from '@/components/save-bar'
import { Section } from '@/components/section'
import { toastWriteSuccess } from '@/features/tasks/write-toast'
import { ClusterAccessFields, ClusterBasicFields } from './cluster-fields'
import {
  clusterFormSchema,
  clusterFormValues,
  type ClusterFormValues,
} from './cluster-form'
import { afterClusterWrite, clusterSnapshotQuery, saveCluster } from './data'
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
  const draft = useRevisionDraft({
    server: cluster,
    save: (values: ClusterFormValues, revision) =>
      saveCluster(values, {
        id: cluster.id,
        revision: revision ?? cluster.revision,
      }),
    onSaved: (response, values) => {
      toastWriteSuccess(response.message)
      form.reset(values)
      void afterClusterWrite(client)
    },
    load: (latest) => form.reset(clusterFormValues(latest ?? cluster)),
    reload,
  })
  const base = draft.base ?? cluster
  const form = useForm<ClusterFormValues>({
    resolver: zodResolver(clusterFormSchema),
    values: clusterFormValues(base),
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
        aria-label='集群配置'
        className='flex flex-col gap-6'
        onSubmit={save}
      >
        <Section title='基本信息'>
          <div className='flex flex-col gap-4'>
            <ClusterBasicFields form={form} disabled={draft.isSaving} />
          </div>
        </Section>
        <Section title='接入域名'>
          <div className='flex flex-col gap-4'>
            <ClusterAccessFields
              form={form}
              disabled={draft.isSaving}
              cluster={base}
            />
          </div>
        </Section>
      </form>
      <SaveBar
        isDirty={isDirty}
        isSaving={draft.isSaving}
        onDiscard={draft.discard}
        onSave={save}
        saveLabel='保存'
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

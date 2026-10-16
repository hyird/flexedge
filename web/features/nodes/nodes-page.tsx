import { getRouteApi, useNavigate } from '@tanstack/react-router'
import { useListSearch } from '@/lib/list-search'
import { Page } from '@/components/page'
import { NodesTable } from './nodes-table'

const route = getRouteApi('/_authenticated/nodes/')

export function NodesPage() {
  const list = useListSearch(route)
  const navigate = useNavigate()
  return (
    <Page
      title='节点'
      description='边缘节点的接入状态与实时负载。CPU、内存为占用比例，带宽按 bit/s 计。'
    >
      <NodesTable
        list={list}
        isCreating={list.search.create === 1}
        onCreatingChange={(creating) =>
          void navigate({
            to: '.',
            search: (previous: Record<string, unknown>) => ({
              ...previous,
              create: creating ? (1 as const) : undefined,
            }),
            replace: !creating,
          })
        }
      />
    </Page>
  )
}

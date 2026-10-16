import { getRouteApi } from '@tanstack/react-router'
import { useListSearch } from '@/lib/list-search'
import { Page } from '@/components/page'
import { NodesTable } from './nodes-table'

const route = getRouteApi('/_authenticated/nodes/')

export function NodesPage() {
  const list = useListSearch(route)
  return (
    <Page
      title='节点'
      description='边缘节点的接入状态与实时负载。CPU、内存为占用比例，带宽按 bit/s 计。'
    >
      <NodesTable
        list={list}
        isCreating={list.sheet.isCreating}
        onCreatingChange={(creating) =>
          creating ? list.sheet.openCreate() : list.sheet.close()
        }
      />
    </Page>
  )
}

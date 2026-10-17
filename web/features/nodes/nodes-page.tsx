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
      description='边缘节点的接入状态与实时负载。每 5 秒更新负载。CPU 显示逻辑核数，内存、磁盘显示总容量，带宽按 bit/s 计。'
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

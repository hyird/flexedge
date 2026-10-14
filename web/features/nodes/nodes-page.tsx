import { getRouteApi, useNavigate } from '@tanstack/react-router'
import { Plus } from '@gravity-ui/icons'
import { Button } from '@heroui/react'
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
      actions={
        <Button
          size='sm'
          onPress={() =>
            void navigate({
              to: '.',
              search: (previous: Record<string, unknown>) => ({
                ...previous,
                create: 1 as const,
              }),
            })
          }
        >
          <Plus className='size-4' />
          添加节点
        </Button>
      }
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

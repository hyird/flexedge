import { getRouteApi } from '@tanstack/react-router'
import { useListSearch } from '@/lib/list-search'
import { Page } from '@/components/page'
import { WebsitesTable } from './websites-table'

const route = getRouteApi('/_authenticated/websites/')

export function WebsitesPage() {
  const list = useListSearch(route)
  return (
    <Page
      title='网站'
      description='接入边缘节点的网站：域名、源站、HTTPS 与发布状态。'
    >
      <WebsitesTable list={list} />
    </Page>
  )
}

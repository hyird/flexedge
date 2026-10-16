import { createFileRoute, redirect } from '@tanstack/react-router'
import { Globe, Layers, Server, ShieldCheck, Signal } from '@gravity-ui/icons'
import { isUnauthorized } from '@/lib/api'
import { AppShell } from '@/components/layout/app-shell'
import type { ResourceSearchSource } from '@/components/layout/command-palette'
import { AccountMenu } from '@/features/auth/account-menu'
import { ensureAuthenticatedSession } from '@/features/auth/data'
import { certificateSearchQuery } from '@/features/certificates/data'
import { clusterSearchQuery } from '@/features/clusters/data'
import { dnsZoneSearchQuery } from '@/features/dns-zones/data'
import { nodeSearchQuery } from '@/features/nodes/data'
import { TaskCenter } from '@/features/tasks/task-center'
import { websiteSearchQuery } from '@/features/websites/data'

/** The command palette's resource searches, each owned by its domain. */
const resourceSearch: ResourceSearchSource[] = [
  { label: '网站', icon: Globe, path: '/websites', search: websiteSearchQuery },
  { label: '节点', icon: Server, path: '/nodes', search: nodeSearchQuery },
  {
    label: '集群',
    icon: Layers,
    path: '/clusters',
    search: clusterSearchQuery,
  },
  {
    label: '证书',
    icon: ShieldCheck,
    path: '/certificates',
    search: certificateSearchQuery,
  },
  {
    label: '域名',
    icon: Signal,
    path: '/dns-zones',
    search: dnsZoneSearchQuery,
  },
]

export const Route = createFileRoute('/_authenticated')({
  beforeLoad: async ({ context, location }) => {
    try {
      return { user: await ensureAuthenticatedSession(context.queryClient) }
    } catch (error) {
      if (isUnauthorized(error))
        throw redirect({
          to: '/sign-in',
          search: { redirect: location.href },
          replace: true,
        })
      throw error
    }
  },
  component: AuthenticatedLayout,
})

function AuthenticatedLayout() {
  return (
    <AppShell
      account={<AccountMenu />}
      taskCenter={<TaskCenter />}
      resourceSearch={resourceSearch}
    />
  )
}

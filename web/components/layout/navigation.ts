import {
  Globe,
  House,
  Layers,
  ListCheck,
  PlugConnection,
  Server,
  ShieldCheck,
  Signal,
} from '@gravity-ui/icons'

type NavigationItem = {
  href: string
  /** Page title everywhere: sidebar, breadcrumb root and command palette. */
  label: string
  icon: typeof House
  group: NavigationGroup
  /** Extra palette search terms. */
  keywords?: string
}
type NavigationGroup = '工作台' | '业务' | '基础设施' | '集成'

export const navigationGroups: NavigationGroup[] = [
  '工作台',
  '业务',
  '基础设施',
  '集成',
]

export const navigation: NavigationItem[] = [
  {
    href: '/',
    label: '概览',
    icon: House,
    group: '工作台',
    keywords: 'dashboard overview',
  },
  {
    href: '/tasks',
    label: '任务中心',
    icon: ListCheck,
    group: '工作台',
    keywords: 'tasks',
  },
  {
    href: '/websites',
    label: '网站',
    icon: Globe,
    group: '业务',
    keywords: 'websites sites',
  },
  {
    href: '/certificates',
    label: '证书',
    icon: ShieldCheck,
    group: '业务',
    keywords: 'certificates tls ssl',
  },
  {
    href: '/clusters',
    label: '集群',
    icon: Layers,
    group: '基础设施',
    keywords: 'clusters',
  },
  {
    href: '/nodes',
    label: '节点',
    icon: Server,
    group: '基础设施',
    keywords: 'nodes edge',
  },
  {
    href: '/dns-zones',
    label: 'DNS 区域',
    icon: Signal,
    group: '基础设施',
    keywords: 'dns zones',
  },
  {
    href: '/providers',
    label: '供应商账号',
    icon: PlugConnection,
    group: '集成',
    keywords: 'providers accounts',
  },
]

/** The navigation item owning `pathname`; child routes (`/websites/x/settings`)
 * resolve to their section (`/websites`). */
export function navigationItemFor(pathname: string) {
  const path = pathname.replace(/\/+$/, '') || '/'
  return path === '/'
    ? navigation[0]
    : navigation.find(
        (item) =>
          item.href !== '/' &&
          (path === item.href || path.startsWith(`${item.href}/`))
      )
}

import {
  House,
  Layers,
  Server,
  Globe,
  Link,
  ShieldCheck,
  Database,
  PlugConnection,
  ListCheck,
} from '@gravity-ui/icons'

export const navigation = [
  { href: '/', label: '概览', icon: House, group: '工作台' },
  { href: '/clusters', label: '集群', icon: Layers, group: '边缘网络' },
  { href: '/nodes', label: '节点', icon: Server, group: '边缘网络' },
  { href: '/websites', label: '网站', icon: Globe, group: '边缘网络' },
  { href: '/dns-zones', label: 'DNS 区域', icon: Link, group: '资源管理' },
  {
    href: '/certificates',
    label: '证书',
    icon: ShieldCheck,
    group: '资源管理',
  },
  {
    href: '/cache-policies',
    label: '缓存策略',
    icon: Database,
    group: '资源管理',
  },
  {
    href: '/providers',
    label: '供应商',
    icon: PlugConnection,
    group: '资源管理',
  },
  { href: '/tasks', label: '任务', icon: ListCheck, group: '运维' },
]

import {
  Activity,
  Boxes,
  ClipboardList,
  CloudCog,
  Globe2,
  Network,
  ShieldCheck,
} from 'lucide-react'
import type { SidebarData } from '../types'

export const sidebarData: SidebarData = {
  user: {
    name: 'FlexEdge 管理员',
    email: '',
    avatar: '',
  },
  teams: [],
  navGroups: [
    {
      title: '总览',
      items: [
        {
          title: '运行概览',
          url: '/',
          icon: Activity,
        },
        {
          title: '后台任务',
          url: '/tasks',
          icon: ClipboardList,
        },
      ],
    },
    {
      title: '边缘资源',
      items: [
        {
          title: '网站',
          url: '/websites',
          icon: Globe2,
        },
        {
          title: '集群与节点',
          url: '/clusters',
          icon: Boxes,
        },
      ],
    },
    {
      title: '域名与安全',
      items: [
        {
          title: 'DNS 托管',
          url: '/dns-zones',
          icon: Network,
        },
        {
          title: '证书',
          url: '/certificates',
          icon: ShieldCheck,
        },
        {
          title: '服务商',
          url: '/providers',
          icon: CloudCog,
        },
      ],
    },
  ],
}

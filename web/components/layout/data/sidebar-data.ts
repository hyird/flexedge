import {
  Boxes,
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
      title: '边缘资源',
      items: [
        {
          title: '集群管理',
          url: '/clusters',
          icon: Boxes,
        },
        {
          title: '网站管理',
          url: '/websites',
          icon: Globe2,
        },
      ],
    },
    {
      title: '域名与证书',
      items: [
        {
          title: '服务商',
          url: '/providers',
          icon: CloudCog,
        },
        {
          title: '域名管理',
          url: '/dns-zones',
          icon: Network,
        },
        {
          title: '证书管理',
          url: '/certificates',
          icon: ShieldCheck,
        },
      ],
    },
  ],
}

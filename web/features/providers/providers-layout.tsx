import { Outlet } from '@tanstack/react-router'
import { Globe, ShieldCheck } from '@gravity-ui/icons'
import { Page } from '@/components/page'
import { RouteTabs } from '@/components/route-tabs'

export function ProvidersLayout() {
  return (
    <Page
      title='供应商账号'
      description='管理托管 DNS 区域所用的 DNS 账号，以及签发证书所用的证书供应商凭据。'
    >
      <RouteTabs
        label='供应商类型'
        items={[
          { href: '/providers/dns', label: 'DNS 账号', icon: <Globe /> },
          {
            href: '/providers/certificate',
            label: '证书供应商',
            icon: <ShieldCheck />,
          },
        ]}
      />
      <Outlet />
    </Page>
  )
}

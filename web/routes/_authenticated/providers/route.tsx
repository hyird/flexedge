import { createFileRoute, Outlet, useLocation } from '@tanstack/react-router'
import { Globe, ShieldCheck } from '@gravity-ui/icons'
import { Tabs } from '@heroui/react'
import { Page } from '@/components/page'

const tabs = [
  {
    id: 'dns',
    href: '/providers/dns',
    label: 'DNS',
    icon: <Globe />,
  },
  {
    id: 'certificate',
    href: '/providers/certificate',
    label: '证书',
    icon: <ShieldCheck />,
  },
] as const

export const Route = createFileRoute('/_authenticated/providers')({
  /** Both provider lists under one page title. Each tab is a link to its list
   * route, so switching tabs navigates and the selected tab follows the URL. */
  component: function ProvidersLayout() {
    const pathname = useLocation({ select: (location) => location.pathname })
    const selected =
      tabs.find(({ href }) => pathname.startsWith(href))?.id ?? 'dns'
    return (
      <Page
        title='服务商'
        description='管理域名解析和证书签发所用的服务商账号。'
      >
        <Tabs
          variant='secondary'
          selectedKey={selected}
          className='-mt-2 flex w-full min-w-0 flex-1 flex-col gap-6'
        >
          <Tabs.ListContainer className='shrink-0'>
            <Tabs.List aria-label='服务商类型'>
              {tabs.map((tab) => (
                <Tabs.Tab
                  key={tab.id}
                  id={tab.id}
                  href={tab.href}
                  className='w-auto flex-none px-3'
                >
                  <span className='flex items-center gap-1.5 [&_svg]:size-4'>
                    {tab.icon}
                    {tab.label}
                  </span>
                  <Tabs.Indicator />
                </Tabs.Tab>
              ))}
            </Tabs.List>
          </Tabs.ListContainer>
          <Tabs.Panel
            id={selected}
            className='flex min-w-0 flex-1 flex-col gap-6 p-0'
          >
            <Outlet />
          </Tabs.Panel>
        </Tabs>
      </Page>
    )
  },
})

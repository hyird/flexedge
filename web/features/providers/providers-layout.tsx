import { getRouteApi } from '@tanstack/react-router'
import { Globe, ShieldCheck } from '@gravity-ui/icons'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { CertificateProvidersPage } from './certificate-providers-page'
import { DnsProvidersPage } from './dns-providers-page'

const route = getRouteApi('/_authenticated/providers/')
type ProvidersTab = 'dns' | 'certificate'
const tabs = [
  { id: 'dns', label: 'DNS 账号', icon: <Globe /> },
  { id: 'certificate', label: '证书供应商', icon: <ShieldCheck /> },
] as const

export function ProvidersLayout() {
  const navigate = route.useNavigate()
  const tab: ProvidersTab =
    route.useSearch({ select: (search) => search.tab }) ?? 'dns'
  const selectTab = (next: ProvidersTab) =>
    void navigate({
      // q/page/page_size/create are the current tab's list state.
      search: (previous) => ({
        ...previous,
        tab: next === 'dns' ? undefined : next,
        q: undefined,
        page: undefined,
        page_size: undefined,
        create: undefined,
      }),
      replace: true,
    })
  return (
    <Page
      title='供应商账号'
      description='管理托管 DNS 区域所用的 DNS 账号，以及签发证书所用的证书供应商凭据。'
    >
      <PageTabs
        label='供应商类型'
        items={tabs}
        selected={tab}
        onSelectionChange={selectTab}
      >
        {tab === 'certificate' ? (
          <CertificateProvidersPage />
        ) : (
          <DnsProvidersPage />
        )}
      </PageTabs>
    </Page>
  )
}

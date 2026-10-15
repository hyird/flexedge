import { Globe, ShieldCheck } from '@gravity-ui/icons'
import { useArrivalState } from '@/lib/history-state'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { CertificateProvidersPage } from './certificate-providers-page'
import { DnsProvidersPage } from './dns-providers-page'

type ProvidersTab = 'dns' | 'certificate'
const tabs = [
  { id: 'dns', label: 'DNS 账号', icon: <Globe /> },
  { id: 'certificate', label: '证书供应商', icon: <ShieldCheck /> },
] as const

/** `/providers` with in-page tabs kept in component state (DNS 账号 by
 * default). Links open a tab, optionally with a starting search keyword,
 * through the `providers` history state; the URL never changes. */
export function ProvidersLayout() {
  const [view, setView, arrivalKey] = useArrivalState((state) => {
    const requested = tabs.find(({ id }) => id === state.providers?.tab)
    const q = state.providers?.q
    return {
      tab: (requested?.id ?? 'dns') as ProvidersTab,
      // Only the tab opened by the arrival starts with its keyword.
      q: typeof q === 'string' ? q : undefined,
    }
  })
  const selectTab = (tab: ProvidersTab) => setView({ tab, q: undefined })
  return (
    <Page
      title='供应商账号'
      description='管理托管域名所用的 DNS 账号，以及签发证书所用的证书供应商凭据。'
    >
      <PageTabs
        label='供应商类型'
        items={tabs}
        selected={view.tab}
        onSelectionChange={selectTab}
      >
        {/* A new arrival remounts the list so it starts from its keyword. */}
        {view.tab === 'certificate' ? (
          <CertificateProvidersPage key={arrivalKey} initialQuery={view.q} />
        ) : (
          <DnsProvidersPage key={arrivalKey} initialQuery={view.q} />
        )}
      </PageTabs>
    </Page>
  )
}

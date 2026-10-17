import { useState } from 'react'
import { useLocation } from '@tanstack/react-router'
import { Globe, ShieldCheck } from '@gravity-ui/icons'
import { Page } from '@/components/page'
import { PageTabs } from '@/components/page-tabs'
import { CertificateProvidersPage } from './certificate-providers-page'
import { DnsProvidersPage } from './dns-providers-page'

const tabs = [
  { id: 'dns', label: 'DNS', icon: <Globe /> },
  { id: 'certificate', label: '证书', icon: <ShieldCheck /> },
] as const

export function ProvidersPage() {
  const initial = useLocation({
    select: (location) => location.state.providers?.tab,
  })
  const [tab, setTab] = useState<'dns' | 'certificate'>(initial ?? 'dns')
  return (
    <Page title='服务商'>
      <PageTabs
        label='服务商类型'
        items={tabs}
        selected={tab}
        onSelectionChange={setTab}
      >
        {tab === 'dns' ? <DnsProvidersPage /> : <CertificateProvidersPage />}
      </PageTabs>
    </Page>
  )
}

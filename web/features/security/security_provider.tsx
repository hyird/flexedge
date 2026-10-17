import type { ReactNode } from 'react'
import { apiErrorMessage } from '@/lib/api'
import { Page, QueryNotice } from '@/components/page'
import { SecurityDataContext, useSecuritySource } from './security_data'

export function SecurityDataProvider({ children }: { children: ReactNode }) {
  const data = useSecuritySource()
  if (!data.core.data)
    return (
      <Page title='安全'>
        <QueryNotice
          query={data.core}
          title='安全配置加载失败'
          onRetry={() => data.core.refetch()}
        />
      </Page>
    )
  return (
    <SecurityDataContext value={data}>
      {data.telemetry.isError && (
        <p role='alert' className='px-6 pt-4 text-sm text-danger'>
          {apiErrorMessage(data.telemetry.error)}
        </p>
      )}
      {children}
    </SecurityDataContext>
  )
}

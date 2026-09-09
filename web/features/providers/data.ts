import { queryOptions } from '@tanstack/react-query'
import { sendData, streamPath, type PageData } from '@/lib/api'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import type {
  CertificateProvider,
  DnsProvider,
  DnsProviderInput,
  CertificateProviderInput,
  ProviderTarget,
} from './types'

type DnsProviderFilters = { page: number; page_size: number; keyword?: string }

export function dnsProvidersQuery(params: DnsProviderFilters) {
  return queryOptions({
  retry: false,
    queryKey: [...queryKeys.providers, 'dns', 'list', params],
    queryFn: ({ queryKey, signal }) => readLiveQuery<PageData<DnsProvider>>(streamPath('/providers/dns', params), queryKey, signal),
  })
}

export const dnsProviderOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.providers, 'dns', 'options'],
  queryFn: ({ queryKey, signal }) => readLiveQuery<DnsProvider[]>(streamPath('/providers/dns/options'), queryKey, signal),
})

export const certificateProvidersQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.providers, 'certificate'],
  queryFn: ({ queryKey, signal }) => readLiveQuery<CertificateProvider[]>(streamPath('/providers/certificate/options'), queryKey, signal),
})

export function saveDnsProvider(
  values: DnsProviderInput,
  current?: Pick<DnsProvider, 'id' | 'revision'>
) {
  return current
    ? sendData(
        'put',
        `/providers/dns/${current.id}`,
        {
          name: values.name,
          ...(values.api_token ? { api_token: values.api_token } : {}),
        },
        current.revision
      )
    : sendData('post', '/providers/dns', values)
}

export function saveCertificateProvider(
  values: CertificateProviderInput,
  current?: Pick<CertificateProvider, 'id' | 'revision'>
) {
  const body = {
    credential_mode: values.credential_mode,
    ...(values.credential_mode === 'email'
      ? { account_email: values.account_email }
      : values.access_key
        ? { access_key: values.access_key }
        : {}),
  }
  return current
    ? sendData(
        'put',
        `/providers/certificate/${current.id}`,
        body,
        current.revision
      )
    : sendData('post', '/providers/certificate', {
        provider: values.provider,
        ...body,
      })
}

export function verifyProvider(target: ProviderTarget) {
  return sendData(
    'post',
    `/providers/${target.kind}/${target.id}/verify`,
    undefined,
    target.revision
  )
}

export function removeProvider(target: ProviderTarget) {
  return sendData(
    'delete',
    `/providers/${target.kind}/${target.id}`,
    undefined,
    target.revision
  )
}

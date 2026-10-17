import { z } from 'zod'
import { queryOptions, type QueryClient } from '@tanstack/react-query'
import { getData, pageDataSchema, sendData, streamPath } from '@/lib/api'
import { parseResponseData } from '@/lib/api-response'
import { readLiveQuery } from '@/lib/live-query'
import { queryKeys } from '@/lib/query-keys'
import {
  certificateProviderSchema,
  dnsProviderSchema,
  type CertificateProvider,
  type CertificateProviderInput,
  type DnsProvider,
  type DnsProviderInput,
  type ProviderTarget,
} from './types'

type ProviderFilters = { page: number; page_size: number; keyword?: string }

const dnsProviderPageSchema = pageDataSchema(dnsProviderSchema)
const dnsProviderListSchema = z.array(dnsProviderSchema)
const certificateProviderPageSchema = pageDataSchema(certificateProviderSchema)
const certificateProviderListSchema = z.array(certificateProviderSchema)

export function dnsProvidersQuery(params: ProviderFilters) {
  return queryOptions({
    retry: false,
    meta: { inlineError: true },
    queryKey: [...queryKeys.providers, 'dns', 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/providers/dns', params),
        queryKey,
        signal,
        (value) => parseResponseData(dnsProviderPageSchema, value)
      ),
  })
}

export const dnsProviderOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.providers, 'dns', 'options'],
  queryFn: async ({ signal }) =>
    parseResponseData(
      dnsProviderListSchema,
      await getData<unknown>('/providers/dns/options', undefined, signal)
    ),
})

/** Paginated GET/SSE contract; every filter is part of the live-query identity. */
export function certificateProvidersQuery(params: ProviderFilters) {
  return queryOptions({
    meta: { inlineError: true },
    retry: false,
    queryKey: [...queryKeys.providers, 'certificate', 'list', params],
    queryFn: ({ queryKey, signal }) =>
      readLiveQuery(
        streamPath('/providers/certificate', params),
        queryKey,
        signal,
        (value) => parseResponseData(certificateProviderPageSchema, value)
      ),
  })
}

/** Complete selector snapshot, independent of the list's URL filters and page. */
export const certificateProviderOptionsQuery = queryOptions({
  retry: false,
  queryKey: [...queryKeys.providers, 'certificate', 'options'],
  queryFn: async ({ signal }) =>
    parseResponseData(
      certificateProviderListSchema,
      await getData<unknown>(
        '/providers/certificate/options',
        undefined,
        signal
      )
    ),
})

/** After any write to a `kind` provider: active list streams receive the
 * change themselves, inactive views and pickers refetch when next shown. */
export function afterProviderWrite(
  client: QueryClient,
  kind: ProviderTarget['kind']
) {
  return client.invalidateQueries({
    queryKey: [...queryKeys.providers, kind],
    refetchType: 'none',
  })
}

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

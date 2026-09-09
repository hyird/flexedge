import axios, { type RawAxiosRequestHeaders } from 'axios'
import { createApiClient } from './api-client'
import { requireResponseData, type ApiResponse } from './api-response'

export type PageData<T> = {
  list: T[]
  total: number
  page: number
  page_size: number
  total_pages: number
}

export const api = createApiClient()

export function normalizeApiPath(url: string) {
  const normalized = url.replace(/\/+((?:[?#].*)?)$/, '$1')
  return normalized || '/'
}

export function streamPath(url: string, params?: Record<string, unknown>) {
  const apiPath = normalizeApiPath(url).replace(/\/$/, '')
  const path = (apiPath.startsWith('/api/') ? apiPath : `/api${apiPath}`) + '/stream'
  if (!params) return path
  const query = new URLSearchParams()
  for (const [key, value] of Object.entries(params).sort(([left], [right]) => left.localeCompare(right)))
    if (value !== undefined && value !== '') query.set(key, String(value))
  const encoded = query.toString()
  return encoded ? `${path}?${encoded}` : path
}

export async function getData<T>(
  url: string,
  params?: Record<string, unknown>,
  signal?: AbortSignal
) {
  const response = await api.get<ApiResponse<T>>(normalizeApiPath(url), {
    params,
    signal,
  })
  return requireResponseData(response.data)
}

export async function sendData<T = never>(
  method: 'post' | 'put' | 'delete',
  url: string,
  data?: unknown,
  revision?: number
) {
  const headers: RawAxiosRequestHeaders | undefined =
    revision === undefined ? undefined : { 'If-Match': `"${revision}"` }
  const response = await api.request<ApiResponse<T>>({
    method,
    url: normalizeApiPath(url),
    data,
    headers,
  })
  return response.data
}

export function apiErrorMessage(error: unknown) {
  if (axios.isAxiosError<ApiResponse>(error)) {
    const message = error.response?.data?.message
    if (typeof message === 'string' && message) return message
    if (error.code === 'ECONNABORTED') return '请求超时，请稍后重试'
    if (!error.response) return '无法连接 FlexEdge 服务'
    if (error.response.status === 401) return '登录状态已失效，请重新登录'
    if (error.response.status === 403) return '当前账户没有操作权限'
    if (error.response.status === 409) return '数据已更新，请刷新后重试'
    if (error.response.status >= 500) return 'FlexEdge 服务暂时不可用'
    if (error.response.status >= 400) return '请求未能完成，请检查输入后重试'
  }
  if (error instanceof Error && error.message) return error.message
  return '操作失败，请稍后重试'
}

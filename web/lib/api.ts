import { z } from 'zod'
import axios, { type RawAxiosRequestHeaders } from 'axios'
import { createApiClient } from './api-client'
import {
  ApiBusinessError,
  requireResponseData,
  type ApiResponse,
} from './api-response'

export type PageData<T> = {
  list: T[]
  total: number
  page: number
  page_size: number
  total_pages: number
}

/** Runtime schema of a `PageData` page of `item`. */
export function pageDataSchema<T extends z.ZodType>(item: T) {
  return z.object({
    list: z.array(item),
    total: z.number().int(),
    page: z.number().int(),
    page_size: z.number().int(),
    total_pages: z.number().int(),
  })
}

export const api = createApiClient()

export function normalizeApiPath(url: string) {
  const normalized = url.replace(/\/+((?:[?#].*)?)$/, '$1')
  return normalized || '/'
}

export function streamPath(url: string, params?: Record<string, unknown>) {
  const apiPath = normalizeApiPath(url).replace(/\/$/, '')
  const path =
    (apiPath.startsWith('/api/') ? apiPath : `/api${apiPath}`) + '/stream'
  if (!params) return path
  const query = new URLSearchParams()
  for (const [key, value] of Object.entries(params).sort(([left], [right]) =>
    left.localeCompare(right)
  ))
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

/** Business code of a failed read: a stream's `resource-error` frame or the
 * envelope of an HTTP error response. */
export function apiErrorCode(error: unknown) {
  if (error instanceof ApiBusinessError) return error.code
  if (axios.isAxiosError<ApiResponse>(error)) {
    const code = error.response?.data?.code
    if (typeof code === 'number') return code
  }
  return undefined
}

/** Result of a write: the response envelope plus the resource revision the
 * write produced (the `ETag` of creates, updates and deletes). */
export type WriteResult<T> = ApiResponse<T> & { revision?: number }

export async function sendData<T = never>(
  method: 'post' | 'put' | 'delete',
  url: string,
  data?: unknown,
  revision?: number
): Promise<WriteResult<T>> {
  const headers: RawAxiosRequestHeaders | undefined =
    revision === undefined ? undefined : { 'If-Match': `"${revision}"` }
  const response = await api.request<ApiResponse<T>>({
    method,
    url: normalizeApiPath(url),
    data,
    headers,
  })
  const etag = /^"(\d+)"$/.exec(String(response.headers.etag ?? ''))
  return { ...response.data, revision: etag ? Number(etag[1]) : undefined }
}

export function apiErrorMessage(error: unknown) {
  if (axios.isAxiosError<ApiResponse>(error)) {
    const message = error.response?.data?.message
    if (typeof message === 'string' && message) return message
    if (error.code === 'ECONNABORTED') return '请求超时，请稍后重试'
    if (!error.response) return '无法连接服务'
    if (error.response.status === 401) return '登录状态已失效，请重新登录'
    if (error.response.status === 403) return '当前账户没有操作权限'
    if (isRevisionConflict(error)) return '数据已被其他人更新，请重新加载后重试'
    if (error.response.status >= 500) return '服务暂时不可用'
    if (error.response.status >= 400) return '请求未能完成，请检查输入后重试'
  }
  if (error instanceof Error && error.message) return error.message
  return '操作失败，请稍后重试'
}

/** True when a write lost a revision race: the backend answers a stale
 * `If-Match` with 412 (e.g. 16205/16016) and some conflicts with 409. UI shows
 * “数据已被其他人更新” and offers to reload the resource. */
export function isRevisionConflict(error: unknown) {
  return (
    axios.isAxiosError(error) &&
    (error.response?.status === 409 || error.response?.status === 412)
  )
}

/** True when the API answered 401: the session is gone (after the client's
 * own refresh attempt). */
export function isUnauthorized(error: unknown) {
  return axios.isAxiosError(error) && error.response?.status === 401
}

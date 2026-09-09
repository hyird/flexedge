import { z } from 'zod'

export type ApiResponse<T = unknown> = {
  code: number
  message: string
  data?: T
}
export type ApiEnvelope<T> = ApiResponse<T> & { data: T }

export class ApiProtocolError extends Error {
  constructor() {
    super('FlexEdge 服务响应格式不正确')
    this.name = 'ApiProtocolError'
  }
}

export class ApiBusinessError extends Error {
  constructor(
    readonly code: number,
    message: string
  ) {
    super(message || '操作失败，请稍后重试')
    this.name = 'ApiBusinessError'
  }
}

const responseSchema = z.object({
  code: z.number().int(),
  message: z.string(),
  data: z.unknown().optional(),
})

export function parseApiResponse(value: unknown): ApiResponse {
  const result = responseSchema.safeParse(value)
  if (!result.success) throw new ApiProtocolError()
  if (result.data.code !== 0)
    throw new ApiBusinessError(result.data.code, result.data.message)
  return result.data
}

export function requireResponseData<T>(response: ApiResponse<T>): T {
  if (response.data === undefined) throw new ApiProtocolError()
  return response.data
}

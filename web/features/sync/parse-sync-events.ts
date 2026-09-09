import { parseApiResponse, requireResponseData } from '@/lib/api-response'
import { syncEventPageSchema } from './types'

export function parseSyncEvents(raw: string) {
  return syncEventPageSchema.parse(
    requireResponseData(parseApiResponse(JSON.parse(raw)))
  ).list
}

import { z } from 'zod'
import { parseApiResponse, requireResponseData } from '@/lib/api-response'

export function createLogEventParser<T>(entry: z.ZodType<T>) {
  const data = z.object({ list: z.array(entry) })
  return (raw: string): T[] =>
    data.parse(requireResponseData(parseApiResponse(JSON.parse(raw)))).list
}

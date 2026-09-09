import { z } from 'zod'

export const authUserSchema = z.object({
  id: z.string().min(1),
  username: z.string().min(1),
  nickname: z.string().optional(),
  status: z.string().min(1),
})

export type AuthUser = z.infer<typeof authUserSchema>

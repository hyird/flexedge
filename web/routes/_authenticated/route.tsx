import { isAxiosError } from 'axios'
import { createFileRoute, redirect } from '@tanstack/react-router'
import { AppShell } from '@/components/layout/app-shell'
import { ensureAuthenticatedSession } from '@/features/auth/data'

export const Route = createFileRoute('/_authenticated')({
  beforeLoad: async ({ context, location }) => {
    try {
      return { user: await ensureAuthenticatedSession(context.queryClient) }
    } catch (error) {
      if (isAxiosError(error) && error.response?.status === 401)
        throw redirect({
          to: '/sign-in',
          search: { redirect: location.href },
          replace: true,
        })
      throw error
    }
  },
  component: AppShell,
})

import { createFileRoute, redirect } from '@tanstack/react-router'
import { AuthenticatedLayout } from '@/components/layout/authenticated-layout'
import { ensureAuthenticatedSession } from '@/features/auth/data'

export const Route = createFileRoute('/_authenticated')({
  beforeLoad: async ({ context, location }) => {
    try {
      await ensureAuthenticatedSession(context.queryClient)
    } catch {
      throw redirect({
        to: '/sign-in',
        search: { redirect: location.href },
      })
    }
  },
  component: () => (
    <>
      <AuthenticatedLayout />
    </>
  ),
})

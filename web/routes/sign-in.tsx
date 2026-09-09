import { createFileRoute, redirect } from '@tanstack/react-router'
import { ensureAuthenticatedSession } from '@/features/auth/data'
import { SignIn } from '@/features/auth/sign-in'

export const Route = createFileRoute('/sign-in')({
  validateSearch: (search: Record<string, unknown>) => {
    const candidate =
      typeof search.redirect === 'string' ? search.redirect : undefined
    return {
      redirect:
        candidate?.startsWith('/') && !candidate.startsWith('//')
          ? candidate
          : undefined,
    }
  },
  beforeLoad: async ({ context }) => {
    const authenticated = await ensureAuthenticatedSession(context.queryClient)
      .then(() => true)
      .catch(() => false)

    if (authenticated) throw redirect({ to: '/' })
  },
  component: SignIn,
})

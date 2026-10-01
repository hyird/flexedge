import { createFileRoute } from '@tanstack/react-router'
import { SignInPage } from '@/features/auth/sign-in'

export const Route = createFileRoute('/sign-in')({
  validateSearch: (search: Record<string, unknown>) => ({
    redirect: typeof search.redirect === 'string' ? search.redirect : undefined,
  }),
  component: SignInPage,
})

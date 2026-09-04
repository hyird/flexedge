import { createFileRoute } from '@tanstack/react-router'
import { Providers } from '@/features/providers'

export const Route = createFileRoute('/_authenticated/providers')({
  component: Providers,
})

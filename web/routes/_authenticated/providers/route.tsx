import { createFileRoute } from '@tanstack/react-router'
import { ProvidersLayout } from '@/features/providers/providers-layout'

export const Route = createFileRoute('/_authenticated/providers')({
  component: ProvidersLayout,
})

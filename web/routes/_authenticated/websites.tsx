import { createFileRoute } from '@tanstack/react-router'
import { WebsitesPage } from '@/features/websites'

export const Route = createFileRoute('/_authenticated/websites')({
  validateSearch: (search: Record<string, unknown>) => search,
  component: WebsitesPage,
})

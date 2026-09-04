import { createFileRoute } from '@tanstack/react-router'
import { Websites } from '@/features/websites'

export const Route = createFileRoute('/_authenticated/websites')({
  component: Websites,
})

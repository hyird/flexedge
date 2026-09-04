import { createFileRoute } from '@tanstack/react-router'
import { Certificates } from '@/features/certificates'

export const Route = createFileRoute('/_authenticated/certificates')({
  component: Certificates,
})

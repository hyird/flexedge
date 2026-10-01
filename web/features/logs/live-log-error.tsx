import { Notice } from '@/components/forms'

export function LiveLogError({ error }: { error: string | null }) {
  return <Notice>{error}</Notice>
}

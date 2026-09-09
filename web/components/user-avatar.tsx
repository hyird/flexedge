import { UserRound } from 'lucide-react'
import { cn } from '@/lib/utils'

export function UserAvatar({ className }: { className?: string }) {
  return <UserRound className={cn('size-4 shrink-0', className)} aria-hidden />
}

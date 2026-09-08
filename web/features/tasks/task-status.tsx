import {
  CheckCircle2,
  CircleAlert,
  Clock3,
  Loader2,
  RotateCw,
  CircleMinus,
} from 'lucide-react'
import { Badge } from '@/components/ui/badge'
import { taskStatuses, type Task } from './data'

const icons = {
  queued: Clock3,
  running: Loader2,
  retrying: RotateCw,
  completed: CheckCircle2,
  recovered: CheckCircle2,
  failed: CircleAlert,
  superseded: CircleMinus,
}

export function TaskStatus({ status }: { status: Task['status'] }) {
  const Icon = icons[status]
  return (
    <Badge
      variant={status === 'failed' ? 'destructive' : 'secondary'}
      className='gap-1 whitespace-nowrap'
    >
      <Icon
        className={
          status === 'running' ? 'size-3 motion-safe:animate-spin' : 'size-3'
        }
        aria-hidden='true'
      />
      {taskStatuses[status]}
    </Badge>
  )
}

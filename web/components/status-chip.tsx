import { Chip } from '@heroui/react'
import { describeStatus, type StatusTone } from '@/lib/status'
import { cn } from '@/lib/utils'

/** Dot + text status. `label`/`tone` override the shared mapping when a
 * domain needs its own wording (e.g. task states). */
export function StatusChip({
  status,
  label,
  tone,
}: {
  status?: string
  label?: string
  tone?: StatusTone
}) {
  const described = describeStatus(status)
  const color = tone ?? described.tone
  return (
    <Chip size='sm' variant='soft' color={color} className='gap-1.5'>
      <span
        aria-hidden
        className={cn(
          'size-1.5 shrink-0 rounded-full',
          color === 'success' && 'bg-success',
          color === 'warning' && 'bg-warning',
          color === 'danger' && 'bg-danger',
          color === 'accent' && 'bg-accent',
          color === 'default' && 'bg-muted'
        )}
      />
      <Chip.Label>{label ?? described.label}</Chip.Label>
    </Chip>
  )
}

import { Chip } from '@heroui/react'
import type { StatusDescriptor, StatusTone } from '@/lib/status'
import { cn } from '@/lib/utils'

function ToneChip({ tone, label }: { tone: StatusTone; label: string }) {
  return (
    <Chip
      size='sm'
      variant='soft'
      color={tone}
      className='shrink-0 gap-1.5 whitespace-nowrap'
    >
      <span
        aria-hidden
        className={cn(
          'size-1.5 shrink-0 rounded-full',
          tone === 'success' && 'bg-success',
          tone === 'warning' && 'bg-warning',
          tone === 'danger' && 'bg-danger',
          tone === 'accent' && 'bg-accent',
          tone === 'default' && 'bg-muted'
        )}
      />
      <Chip.Label>{label}</Chip.Label>
    </Chip>
  )
}

/** Dot + text status on one line that never wraps or shrinks. Pass the
 * descriptor from the owning domain's dictionary (`*-display.ts`), e.g.
 * `<StatusChip {...describeStatus(certificateStatuses, status)} />`.
 * `reserve` lists every label this slot can show (`statusReserve`): the slot
 * keeps the widest one's width, so switching states never moves neighbouring
 * text or changes the column width. */
export function StatusChip({
  label,
  tone,
  reserve,
}: StatusDescriptor & { reserve?: readonly string[] }) {
  const chip = <ToneChip tone={tone} label={label} />
  if (!reserve?.length) return chip
  return (
    <span className='inline-grid shrink-0 justify-items-start align-middle'>
      {reserve.map((reserved) => (
        <span
          key={reserved}
          aria-hidden
          className='invisible col-start-1 row-start-1 flex'
        >
          <ToneChip tone='default' label={reserved} />
        </span>
      ))}
      <span className='col-start-1 row-start-1 flex'>{chip}</span>
    </span>
  )
}

import { type ReactNode } from 'react'
import { ArrowUpRightFromSquare } from '@gravity-ui/icons'
import { Button, Checkbox, Link, Tooltip } from '@heroui/react'
import { Field, Notice } from '@/components/forms'

/** Integer input bound to a number draft value; an empty box is kept as NaN
 * so validation reports it instead of silently writing 0. */
export function NumberField({
  label,
  value,
  onChange,
  error,
  hint,
  min,
  max,
}: {
  label: string
  value: number
  onChange: (value: number) => void
  error?: string
  hint?: string
  min?: number
  max?: number
}) {
  return (
    <Field
      label={label}
      type='number'
      inputMode='numeric'
      value={Number.isNaN(value) ? '' : String(value)}
      onChange={(next) => onChange(next === '' ? Number.NaN : Number(next))}
      error={error}
      hint={hint}
      min={min}
      max={max}
    />
  )
}

/** Compact checkbox for multi-option sets (log fields, status classes, methods). */
export function CheckOption({
  label,
  selected,
  onChange,
  isDisabled,
}: {
  label: ReactNode
  selected: boolean
  onChange: (selected: boolean) => void
  isDisabled?: boolean
}) {
  return (
    <Checkbox
      isSelected={selected}
      onChange={onChange}
      isDisabled={isDisabled}
      variant='secondary'
    >
      <Checkbox.Content className='py-1.5 text-sm'>
        <Checkbox.Control>
          <Checkbox.Indicator />
        </Checkbox.Control>
        {label}
      </Checkbox.Content>
    </Checkbox>
  )
}

/** Group-level validation message (e.g. "至少添加一个域名"). Focusable so a
 * failed save can move focus to it when no single input is at fault. */
export function FieldNotice({ children }: { children?: ReactNode }) {
  if (!children) return null
  return (
    <div data-field-error tabIndex={-1} className='outline-none'>
      <Notice>{children}</Notice>
    </div>
  )
}

/** Link to another console page in a new window, so the unsaved draft here
 * stays intact (create a certificate, inspect a cluster, edit a policy). */
export function NewWindowLink({
  href,
  children,
}: {
  href: string
  children: ReactNode
}) {
  return (
    <Link
      href={href}
      target='_blank'
      rel='noopener noreferrer'
      className='text-[13px]'
    >
      {children}
      <span className='sr-only'>（在新窗口打开）</span>
      <Link.Icon className='ms-1 size-3'>
        <ArrowUpRightFromSquare />
      </Link.Icon>
    </Link>
  )
}

/** Icon-only button inline with form rows (refresh, reorder, remove): quiet
 * `ghost`, with tooltip and accessible name. */
export function IconAction({
  label,
  icon,
  onPress,
  isDisabled,
  isPending,
}: {
  label: string
  icon: ReactNode
  onPress: () => void
  isDisabled?: boolean
  isPending?: boolean
}) {
  return (
    <Tooltip delay={300}>
      <Button
        size='sm'
        variant='ghost'
        isIconOnly
        aria-label={label}
        isDisabled={isDisabled}
        isPending={isPending}
        onPress={onPress}
      >
        {icon}
      </Button>
      <Tooltip.Content>{label}</Tooltip.Content>
    </Tooltip>
  )
}

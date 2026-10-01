import { type ComponentProps, type ReactNode } from 'react'
import {
  Alert,
  Button,
  Description,
  FieldError,
  Input,
  Label,
  ListBox,
  Modal,
  Select,
  Switch,
  TextArea,
  TextField,
} from '@heroui/react'
import { cn } from '@/lib/utils'

type FieldProps = {
  label: string
  value: string
  onChange: (value: string) => void
  hint?: string
  error?: string
  required?: boolean
  disabled?: boolean
  type?: ComponentProps<typeof Input>['type']
  autoComplete?: string
  placeholder?: string
  minLength?: number
  maxLength?: number
  pattern?: string
  min?: number | string
  max?: number | string
  step?: number | string
  inputMode?: ComponentProps<typeof Input>['inputMode']
  className?: string
}

export function Field({
  label,
  value,
  onChange,
  hint,
  error,
  required,
  disabled,
  className,
  ...input
}: FieldProps) {
  return (
    <TextField
      value={value}
      onChange={onChange}
      isRequired={required}
      isDisabled={disabled}
      isInvalid={!!error}
      variant='secondary'
      className={cn('w-full', className)}
    >
      <Label>{label}</Label>
      <Input {...input} className='w-full' />
      {hint && <Description>{hint}</Description>}
      <FieldError>{error}</FieldError>
    </TextField>
  )
}

export function TextAreaField({
  label,
  value,
  onChange,
  hint,
  error,
  rows = 4,
  required,
  disabled,
  placeholder,
  className,
}: Omit<FieldProps, 'type'> & { rows?: number }) {
  return (
    <TextField
      value={value}
      onChange={onChange}
      isRequired={required}
      isDisabled={disabled}
      isInvalid={!!error}
      variant='secondary'
      className={cn('w-full', className)}
    >
      <Label>{label}</Label>
      <TextArea rows={rows} placeholder={placeholder} className='w-full' />
      {hint && <Description>{hint}</Description>}
      <FieldError>{error}</FieldError>
    </TextField>
  )
}

export function Choice({
  label,
  value,
  onChange,
  items,
  disabled,
  compact,
  required,
  error,
  hint,
  className,
}: {
  label: string
  value: string
  onChange: (value: string) => void
  items: { id: string; label: string }[]
  disabled?: boolean
  compact?: boolean
  required?: boolean
  error?: string
  hint?: string
  className?: string
}) {
  const options = items.map((item) => ({ ...item, id: item.id || '__empty__' }))
  return (
    <Select
      aria-label={label}
      value={value || (items.some((item) => !item.id) ? '__empty__' : null)}
      onChange={(key) => {
        if (typeof key === 'string') onChange(key === '__empty__' ? '' : key)
      }}
      isDisabled={disabled}
      isRequired={required}
      isInvalid={!!error}
      variant='secondary'
      placeholder='请选择'
      className={cn(compact ? 'max-w-56 min-w-32' : 'w-full', className)}
    >
      {!compact && <Label>{label}</Label>}
      <Select.Trigger>
        <Select.Value />
        <Select.Indicator />
      </Select.Trigger>
      <Select.Popover>
        <ListBox items={options}>
          {(item) => (
            <ListBox.Item id={item.id} textValue={item.label}>
              {item.label}
              <ListBox.ItemIndicator />
            </ListBox.Item>
          )}
        </ListBox>
      </Select.Popover>
      {hint && <Description>{hint}</Description>}
      {error && <FieldError>{error}</FieldError>}
    </Select>
  )
}

export function Toggle({
  label,
  hint,
  selected,
  onChange,
  disabled,
}: {
  label: string
  hint?: string
  selected: boolean
  onChange: (value: boolean) => void
  disabled?: boolean
}) {
  return (
    <div className='flex items-center justify-between gap-5 py-2'>
      <div className='min-w-0'>
        <p className='text-sm font-medium'>{label}</p>
        {hint && <p className='mt-1 text-xs text-muted'>{hint}</p>}
      </div>
      <Switch
        aria-label={label}
        isSelected={selected}
        onChange={onChange}
        isDisabled={disabled}
        size='sm'
      >
        <Switch.Content>
          <Switch.Control>
            <Switch.Thumb />
          </Switch.Control>
        </Switch.Content>
      </Switch>
    </div>
  )
}

export function Notice({
  children,
  success = false,
}: {
  children: ReactNode
  success?: boolean
}) {
  if (!children) return null
  return (
    <Alert status={success ? 'success' : 'danger'}>
      <Alert.Indicator />
      <Alert.Content>
        <Alert.Description>{children}</Alert.Description>
      </Alert.Content>
    </Alert>
  )
}

export function Dialog({
  title,
  children,
  onClose,
  busy = false,
  size = 'sm',
}: {
  title: string
  children: ReactNode
  onClose: () => void
  busy?: boolean
  size?: 'sm' | 'md' | 'lg'
}) {
  return (
    <Modal.Backdrop
      isOpen
      onOpenChange={(open) => {
        if (!open && !busy) onClose()
      }}
      isDismissable={!busy}
      isKeyboardDismissDisabled={busy}
    >
      <Modal.Container size={size} placement='center' scroll='inside'>
        <Modal.Dialog
          aria-label={title}
          className={cn(
            size === 'lg'
              ? 'sm:max-w-5xl'
              : size === 'md'
                ? 'sm:max-w-2xl'
                : 'sm:max-w-lg'
          )}
        >
          <Modal.CloseTrigger aria-label='关闭对话框' isDisabled={busy} />
          <Modal.Header>
            <Modal.Heading>{title}</Modal.Heading>
          </Modal.Header>
          <Modal.Body className='pb-5'>{children}</Modal.Body>
        </Modal.Dialog>
      </Modal.Container>
    </Modal.Backdrop>
  )
}

export function FormActions({
  onCancel,
  busy,
  label = '保存',
  danger,
}: {
  onCancel: () => void
  busy: boolean
  label?: string
  danger?: boolean
}) {
  return (
    <div className='flex justify-end gap-2 pt-3'>
      <Button size='sm' variant='tertiary' onPress={onCancel} isDisabled={busy}>
        取消
      </Button>
      <Button
        size='sm'
        type='submit'
        variant={danger ? 'danger-soft' : 'primary'}
        isPending={busy}
      >
        {label}
      </Button>
    </div>
  )
}

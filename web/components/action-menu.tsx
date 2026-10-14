import { type ReactNode } from 'react'
import { Ellipsis } from '@gravity-ui/icons'
import { Button, Dropdown, Label, Separator, Tooltip } from '@heroui/react'

export type ActionMenuItem = {
  id: string
  label: string
  icon?: ReactNode
  /** In-app navigation target; preferred over `onAction` for pure links. */
  href?: string
  onAction?: () => void
  danger?: boolean
  disabled?: boolean
  /** Draw a separator above this item (e.g. before the danger group). */
  separated?: boolean
}

/** Row and header overflow menu: an icon button with tooltip and a dropdown.
 * Danger items should come last and be `separated`; destructive actions open
 * a `ConfirmDialog` from `onAction`. */
export function ActionMenu({
  label,
  items,
}: {
  /** Accessible name and tooltip, e.g. `网站 qa.example.test 的操作`. */
  label: string
  items: ActionMenuItem[]
}) {
  return (
    <Dropdown>
      <Tooltip delay={300}>
        <Button size='sm' variant='ghost' isIconOnly aria-label={label}>
          <Ellipsis className='size-4' />
        </Button>
        <Tooltip.Content>{label}</Tooltip.Content>
      </Tooltip>
      <Dropdown.Popover placement='bottom end' className='min-w-44'>
        <Dropdown.Menu
          aria-label={label}
          onAction={(id) => items.find((item) => item.id === id)?.onAction?.()}
        >
          {items.flatMap((item) => [
            ...(item.separated
              ? [<Separator key={`${item.id}-separator`} />]
              : []),
            <Dropdown.Item
              key={item.id}
              id={item.id}
              href={item.href}
              textValue={item.label}
              variant={item.danger ? 'danger' : 'default'}
              isDisabled={item.disabled}
            >
              {item.icon && (
                <span className='text-muted [&_svg]:size-4'>{item.icon}</span>
              )}
              <Label>{item.label}</Label>
            </Dropdown.Item>,
          ])}
        </Dropdown.Menu>
      </Dropdown.Popover>
    </Dropdown>
  )
}

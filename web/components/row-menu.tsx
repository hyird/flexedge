import { Ellipsis } from '@gravity-ui/icons'
import { Button, Dropdown, Label, Tooltip } from '@heroui/react'

export function RowMenu({
  label,
  actions,
}: {
  label: string
  actions: {
    id: string
    label: string
    onAction: () => void
    danger?: boolean
    disabled?: boolean
  }[]
}) {
  return (
    <Dropdown>
      <Tooltip delay={0}>
        <Button size='sm' variant='ghost' isIconOnly aria-label={label}>
          <Ellipsis className='size-4' />
        </Button>
        <Tooltip.Content>{label}</Tooltip.Content>
      </Tooltip>
      <Dropdown.Popover>
        <Dropdown.Menu
          aria-label={label}
          onAction={(id) =>
            actions.find((action) => action.id === id)?.onAction()
          }
        >
          {actions.map((action) => (
            <Dropdown.Item
              key={action.id}
              id={action.id}
              textValue={action.label}
              variant={action.danger ? 'danger' : undefined}
              isDisabled={action.disabled}
            >
              <Label>{action.label}</Label>
            </Dropdown.Item>
          ))}
        </Dropdown.Menu>
      </Dropdown.Popover>
    </Dropdown>
  )
}

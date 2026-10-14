import { Sun } from '@gravity-ui/icons'
import { Button, Dropdown, Label, Tooltip } from '@heroui/react'
import { themeOptions, useTheme } from '@/context/theme-context'

/** Navbar theme switcher (light / dark / system). */
export function ThemeMenu() {
  const { theme, setTheme } = useTheme()
  const Icon = themeOptions.find((option) => option.id === theme)?.icon ?? Sun
  return (
    <Dropdown>
      <Tooltip delay={300}>
        <Button size='sm' variant='ghost' isIconOnly aria-label='切换主题'>
          <Icon className='size-4' />
        </Button>
        <Tooltip.Content>切换主题</Tooltip.Content>
      </Tooltip>
      <Dropdown.Popover placement='bottom end'>
        <Dropdown.Menu
          aria-label='选择主题'
          selectionMode='single'
          selectedKeys={[theme]}
          onAction={(key) => {
            const option = themeOptions.find((item) => item.id === key)
            if (option) setTheme(option.id)
          }}
        >
          {themeOptions.map((option) => (
            <Dropdown.Item
              key={option.id}
              id={option.id}
              textValue={option.label}
            >
              <option.icon className='size-4 text-muted' />
              <Label>{option.label}</Label>
              <Dropdown.ItemIndicator />
            </Dropdown.Item>
          ))}
        </Dropdown.Menu>
      </Dropdown.Popover>
    </Dropdown>
  )
}

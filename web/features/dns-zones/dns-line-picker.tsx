import { useMemo } from 'react'
import {
  ComboBox,
  FieldError,
  Input,
  Label,
  ListBox,
  ListLayout,
  Virtualizer,
} from '@heroui/react'
import { dnsLineIndex } from './dns-lines'
import type { DnsLine } from './types'

type LineItem = { id: string; label: string; isDisabled: boolean }

const rowHeight = 32

/** Searchable DNS line picker over the zone's one-shot line snapshot. The
 * items come from the snapshot's shared index (built once per snapshot) and
 * the open list is virtualized, so a provider list of hundreds of lines
 * (Aliyun) opens and filters without rendering every option. Disabled lines
 * stay visible but cannot be chosen (the backend rejects them); a current
 * code missing from the snapshot is kept as its own item. */
export function DnsLinePicker({
  lines,
  value,
  onChange,
  error,
  disabled,
  label = '解析线路',
}: {
  lines: DnsLine[]
  value: string
  onChange: (code: string) => void
  error?: string
  disabled?: boolean
  label?: string
}) {
  const index = dnsLineIndex(lines)
  const items = useMemo<LineItem[]>(() => {
    const known = index.options.map(({ line }) => ({
      id: line.code,
      label: index.labels.get(line.code) ?? line.code,
      isDisabled: line.status !== 'enabled' && line.code !== value,
    }))
    return !value || index.labels.has(value)
      ? known
      : [{ id: value, label: value, isDisabled: false }, ...known]
  }, [index, value])
  return (
    <ComboBox<LineItem>
      defaultItems={items}
      selectedKey={value || null}
      onSelectionChange={(key) => {
        if (typeof key === 'string') onChange(key)
      }}
      disabledKeys={items.filter((item) => item.isDisabled).map(({ id }) => id)}
      isDisabled={disabled}
      isInvalid={!!error}
      isRequired
      menuTrigger='focus'
      variant='secondary'
      className='w-full'
    >
      <Label>{label}</Label>
      <ComboBox.InputGroup>
        <Input placeholder='搜索线路' />
        <ComboBox.Trigger />
      </ComboBox.InputGroup>
      <ComboBox.Popover className='w-(--trigger-width) overflow-hidden'>
        <Virtualizer layout={ListLayout} layoutOptions={{ rowHeight }}>
          <ListBox<LineItem>
            className='max-h-80 overflow-y-auto'
            renderEmptyState={() => (
              <p className='px-3 py-2 text-sm text-muted'>没有匹配的线路</p>
            )}
          >
            {(item) => (
              <ListBox.Item id={item.id} textValue={item.label} className='h-8'>
                <span className='truncate'>{item.label}</span>
                <ListBox.ItemIndicator />
              </ListBox.Item>
            )}
          </ListBox>
        </Virtualizer>
      </ComboBox.Popover>
      <FieldError>{error}</FieldError>
    </ComboBox>
  )
}

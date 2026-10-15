import {
  type ReactNode,
  useEffect,
  useEffectEvent,
  useRef,
  useState,
} from 'react'
import { ArrowRotateLeft, Xmark } from '@gravity-ui/icons'
import { Button, ListBox, SearchField, Select, Tooltip } from '@heroui/react'

const searchDebounceMs = 300

function isTypingTarget(target: EventTarget | null) {
  return (
    target instanceof HTMLElement &&
    (target.isContentEditable ||
      ['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName))
  )
}

/** Keyword box bound to the URL `q`: commits on Enter or after 300ms idle and
 * follows external URL changes (back/forward). `/` focuses it page-wide. */
function ListSearchField({
  value,
  onChange,
  placeholder,
}: {
  value: string
  onChange: (value: string) => void
  placeholder: string
}) {
  const [draft, setDraft] = useState(value)
  const [committed, setCommitted] = useState(value)
  const input = useRef<HTMLInputElement>(null)
  // Adopt URL changes made elsewhere without an effect round-trip; keep the
  // draft (including a trailing space mid-typing) when it already matches.
  if (value !== committed) {
    setCommitted(value)
    if (value !== draft.trim()) setDraft(value)
  }
  const commit = useEffectEvent((next: string) => onChange(next))
  useEffect(() => {
    const next = draft.trim()
    if (next === value) return
    const timer = window.setTimeout(() => commit(next), searchDebounceMs)
    return () => window.clearTimeout(timer)
  }, [draft, value])
  useEffect(() => {
    const focusSearch = (event: KeyboardEvent) => {
      if (
        event.key !== '/' ||
        event.metaKey ||
        event.ctrlKey ||
        event.altKey ||
        isTypingTarget(event.target)
      )
        return
      event.preventDefault()
      input.current?.focus()
    }
    window.addEventListener('keydown', focusSearch)
    return () => window.removeEventListener('keydown', focusSearch)
  }, [])
  return (
    <SearchField
      aria-label={placeholder}
      value={draft}
      onChange={setDraft}
      onSubmit={() => onChange(draft.trim())}
      onClear={() => onChange('')}
      variant='secondary'
      className='w-full sm:w-72'
    >
      <SearchField.Group>
        <SearchField.SearchIcon />
        <SearchField.Input ref={input} placeholder={`${placeholder}…`} />
        <SearchField.ClearButton aria-label='清空搜索' />
      </SearchField.Group>
    </SearchField>
  )
}

/** List toolbar: search and filters on the left (with "清除筛选" when any
 * filter is active), refresh and primary actions on the right. */
export function ListToolbar({
  search,
  filters,
  hasFilters = false,
  onClearFilters,
  onRefresh,
  isRefreshing = false,
  actions,
}: {
  search?: {
    value: string
    onChange: (value: string) => void
    /** E.g. `搜索名称或域名`. */
    placeholder: string
  }
  /** `FilterSelect` elements. */
  filters?: ReactNode
  hasFilters?: boolean
  onClearFilters?: () => void
  /** Manual refresh (`query.refetch`), which reconnects the stream. */
  onRefresh?: () => unknown
  isRefreshing?: boolean
  actions?: ReactNode
}) {
  return (
    <div className='flex flex-wrap items-center gap-2'>
      {search && <ListSearchField {...search} />}
      {filters}
      {hasFilters && onClearFilters && (
        <Button size='sm' variant='tertiary' onPress={onClearFilters}>
          <Xmark className='size-3.5' />
          清除筛选
        </Button>
      )}
      <div className='ml-auto flex items-center gap-2'>
        {onRefresh && (
          <Tooltip delay={300}>
            <Button
              size='sm'
              variant='tertiary'
              isIconOnly
              aria-label='刷新'
              isPending={isRefreshing}
              onPress={() => {
                onRefresh()
              }}
            >
              <ArrowRotateLeft className='size-4' />
            </Button>
            <Tooltip.Content>刷新</Tooltip.Content>
          </Tooltip>
        )}
        {actions}
      </div>
    </div>
  )
}

/** Compact filter select for toolbars. `value` `''` means "all" and is shown
 * as `allLabel`; the visible label prefixes the current value. */
export function FilterSelect({
  label,
  value,
  onChange,
  options,
  allLabel = '全部',
}: {
  label: string
  value: string
  onChange: (value: string) => void
  options: { id: string; label: string }[]
  allLabel?: string
}) {
  const allKey = '__all__'
  return (
    <Select
      aria-label={label}
      value={value || allKey}
      onChange={(key) => {
        if (typeof key === 'string') onChange(key === allKey ? '' : key)
      }}
      variant='secondary'
      className='w-auto min-w-36'
    >
      <Select.Trigger className='gap-1.5'>
        <span className='shrink-0 text-muted'>{label}</span>
        <Select.Value />
        <Select.Indicator />
      </Select.Trigger>
      <Select.Popover className='min-w-44'>
        <ListBox>
          {[{ id: allKey, label: allLabel }, ...options].map((option) => (
            <ListBox.Item
              key={option.id}
              id={option.id}
              textValue={option.label}
            >
              {option.label}
              <ListBox.ItemIndicator />
            </ListBox.Item>
          ))}
        </ListBox>
      </Select.Popover>
    </Select>
  )
}

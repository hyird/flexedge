import { useRef, useState } from 'react'
import { Command as CommandPrimitive } from 'cmdk'
import { Check, ChevronDown, X } from 'lucide-react'
import { Badge } from '@/components/ui/badge'
import { Command, CommandEmpty, CommandItem, CommandList } from '@/components/ui/command'
import { FormControl } from '@/components/ui/form'
import { Popover, PopoverAnchor, PopoverContent } from '@/components/ui/popover'

type Algorithm = 'br' | 'zstd' | 'gzip'

export function CompressionAlgorithmSelect({ value, onChange }: {
  value: Algorithm[]
  onChange: (value: Algorithm[]) => void
}) {
  const [open, setOpen] = useState(false)
  const [search, setSearch] = useState('')
  const input = useRef<HTMLInputElement>(null)
  const anchor = useRef<HTMLDivElement>(null)
  return (
    <Popover open={open} onOpenChange={setOpen}>
      <Command className='overflow-visible bg-transparent'>
        <PopoverAnchor asChild>
          <div ref={anchor} onClick={() => { setOpen(true); input.current?.focus() }} className='flex min-h-9 w-full flex-wrap items-center gap-1 rounded-md border border-input bg-transparent px-3 py-1 shadow-xs focus-within:border-ring focus-within:ring-[3px] focus-within:ring-ring/50'>
            {value.map((algorithm) => (
              <Badge key={algorithm} variant='secondary' className='gap-1'>
                {algorithm}
                <button type='button' aria-label={`移除 ${algorithm}`} className='rounded-sm outline-none focus-visible:ring-2 focus-visible:ring-ring' onClick={() => onChange(value.filter((item) => item !== algorithm))}>
                  <X className='size-3' />
                </button>
              </Badge>
            ))}
            <FormControl>
              <CommandPrimitive.Input
                ref={input}
                value={search}
                onValueChange={setSearch}
                onFocus={() => setOpen(true)}
                onClick={() => setOpen(true)}
                aria-expanded={open}
                placeholder={value.length ? '' : '选择压缩算法'}
                className='h-7 min-w-16 flex-1 bg-transparent text-sm outline-none placeholder:text-muted-foreground'
                onKeyDown={(event) => {
                  if (event.key === 'ArrowDown') setOpen(true)
                  if (event.key === 'Backspace' && !search && value.length) onChange(value.slice(0, -1))
                }}
              />
            </FormControl>
            <button type='button' aria-label='展开压缩算法选项' className='rounded-sm p-1 text-muted-foreground focus-visible:ring-2 focus-visible:ring-ring' onClick={() => { setOpen(true); input.current?.focus() }}>
              <ChevronDown className='size-4' />
            </button>
          </div>
        </PopoverAnchor>
        <PopoverContent align='start' className='w-(--radix-popover-trigger-width) p-1' onOpenAutoFocus={(event) => event.preventDefault()} onCloseAutoFocus={(event) => event.preventDefault()}
          // The editable input belongs to the anchor, outside the portalled list.
          // Focusing/clicking it must not dismiss its own candidate popup.
          onInteractOutside={(event) => {
            if (event.target instanceof Node && anchor.current?.contains(event.target)) event.preventDefault()
          }}
        >
          <CommandList>
            <CommandEmpty>没有匹配的算法</CommandEmpty>
            {(['br', 'zstd', 'gzip'] as const).map((algorithm) => (
              <CommandItem key={algorithm} value={algorithm} onMouseDown={(event) => event.preventDefault()} onSelect={() => {
                onChange(value.includes(algorithm) ? value.filter((item) => item !== algorithm) : [...value, algorithm])
                setSearch('')
              }}>
                {algorithm}
                {value.includes(algorithm) && <Check className='ml-auto size-4' />}
              </CommandItem>
            ))}
          </CommandList>
        </PopoverContent>
      </Command>
    </Popover>
  )
}

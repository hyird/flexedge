import { PAGE_SIZE_OPTIONS } from '@/lib/page-size'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'

type PageSizeSelectProps = {
  value: number
  onValueChange: (value: number) => void
  disabled?: boolean
}

export function PageSizeSelect({
  value,
  onValueChange,
  disabled = false,
}: PageSizeSelectProps) {
  return (
    <Select
      disabled={disabled}
      value={`${value}`}
      onValueChange={(nextValue) => onValueChange(Number(nextValue))}
    >
      <SelectTrigger className='h-8 w-24' aria-label='每页行数'>
        <SelectValue placeholder={value} />
      </SelectTrigger>
      <SelectContent side='top'>
        {PAGE_SIZE_OPTIONS.map((pageSize) => (
          <SelectItem key={pageSize} value={`${pageSize}`}>
            {pageSize}
          </SelectItem>
        ))}
      </SelectContent>
    </Select>
  )
}

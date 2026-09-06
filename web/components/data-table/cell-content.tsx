import type { ReactNode } from 'react'

// Keep primary and secondary values on one line; the table owns overflow.
export function DataTableCellContent({ children }: { children: ReactNode }) {
  return (
    <div className='inline-flex flex-nowrap items-center gap-2 whitespace-nowrap'>
      {children}
    </div>
  )
}

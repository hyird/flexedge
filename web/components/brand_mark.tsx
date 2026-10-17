import logo from '@/assets/flexedge.svg'

export function BrandMark({ className = 'size-8' }: { className?: string }) {
  return (
    <img
      src={logo}
      alt=''
      aria-hidden='true'
      className={`shrink-0 ${className}`}
    />
  )
}

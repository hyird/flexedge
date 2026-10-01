import { createContext, useContext } from 'react'

export type Theme = 'light' | 'dark' | 'system'
export const ThemeContext = createContext<{
  theme: Theme
  setTheme: (theme: Theme) => void
} | null>(null)
export function useTheme() {
  const value = useContext(ThemeContext)
  if (!value) throw new Error('缺少主题上下文')
  return value
}

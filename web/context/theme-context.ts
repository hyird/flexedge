import { createContext, useContext } from 'react'
import { Display, Moon, Sun } from '@gravity-ui/icons'

export type Theme = 'light' | 'dark' | 'system'

/** Theme choices shown by the navbar theme menu and the account menu. */
export const themeOptions: { id: Theme; label: string; icon: typeof Sun }[] = [
  { id: 'light', label: '浅色', icon: Sun },
  { id: 'dark', label: '深色', icon: Moon },
  { id: 'system', label: '跟随系统', icon: Display },
]

export const ThemeContext = createContext<{
  theme: Theme
  setTheme: (theme: Theme) => void
} | null>(null)
export function useTheme() {
  const value = useContext(ThemeContext)
  if (!value) throw new Error('缺少主题上下文')
  return value
}

import { type ReactNode, useEffect, useState } from 'react'
import { ThemeContext, type Theme } from './theme-context'

function savedTheme(): Theme {
  try {
    const value = localStorage.getItem('flexedge-theme')
    if (value === 'light' || value === 'dark' || value === 'system')
      return value
  } catch {
    /* Storage is optional. */
  }
  return 'light'
}
export function ThemeProvider({ children }: { children: ReactNode }) {
  const [theme, setTheme] = useState<Theme>(savedTheme)
  useEffect(() => {
    const media = window.matchMedia('(prefers-color-scheme: dark)')
    const apply = () => {
      const dark = theme === 'dark' || (theme === 'system' && media.matches)
      const root = document.documentElement
      root.classList.toggle('dark', dark)
      root.classList.toggle('light', !dark)
      root.dataset.theme = dark ? 'dark' : 'light'
      root.style.colorScheme = dark ? 'dark' : 'light'
    }
    apply()
    media.addEventListener('change', apply)
    try {
      localStorage.setItem('flexedge-theme', theme)
    } catch {
      /* Storage is optional. */
    }
    return () => media.removeEventListener('change', apply)
  }, [theme])
  return (
    <ThemeContext.Provider value={{ theme, setTheme }}>
      {children}
    </ThemeContext.Provider>
  )
}

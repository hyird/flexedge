import { fileURLToPath } from 'node:url'
import { Generator, getConfig } from '@tanstack/router-generator'

const root = fileURLToPath(new URL('..', import.meta.url))
await new Generator({ config: getConfig({}, root), root }).run()

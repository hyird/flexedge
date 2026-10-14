import { z } from 'zod'
import { describe, expect, it } from 'vitest'
import {
  applyListSearchPatch,
  hasListFilters,
  listSearchSchema,
  optionalSearchValue,
  pageLocally,
} from './list-search'
import { pageWindow } from './pagination'

const websiteSearchSchema = listSearchSchema.extend({
  status: optionalSearchValue(z.enum(['enabled', 'disabled'])),
})

describe('list search schema', () => {
  it('drops invalid and default values instead of failing the route', () => {
    expect(
      websiteSearchSchema.parse({
        q: '  ',
        page: 1,
        page_size: 20,
        status: 'archived',
      })
    ).toEqual({})
    expect(
      websiteSearchSchema.parse({ page: 'x', page_size: 30, status: 1 })
    ).toEqual({})
  })

  it('keeps meaningful values and accepts numeric keywords typed by hand', () => {
    expect(
      websiteSearchSchema.parse({
        q: 123,
        page: 3,
        page_size: 50,
        status: 'enabled',
      })
    ).toEqual({ q: '123', page: 3, page_size: 50, status: 'enabled' })
  })
})

describe('list search updates', () => {
  it('returns to the first page when a filter changes', () => {
    expect(
      applyListSearchPatch(
        { q: 'qa', page: 4, status: 'enabled' },
        { status: 'disabled' }
      )
    ).toEqual({ q: 'qa', status: 'disabled' })
  })

  it('keeps filters when paging and omits defaults from the URL', () => {
    expect(applyListSearchPatch({ q: 'qa', page: 4 }, { page: 1 })).toEqual({
      q: 'qa',
    })
    expect(
      applyListSearchPatch({ page: 2, page_size: 50 }, { page_size: 20 })
    ).toEqual({})
  })

  it('clears a filter with an empty value and keeps unrelated params', () => {
    expect(applyListSearchPatch({ q: 'qa', task: 't-1' }, { q: '' })).toEqual({
      task: 't-1',
    })
  })

  it('detects active filters including the keyword', () => {
    const idle: { page?: number; status?: string; q?: string } = { page: 3 }
    expect(hasListFilters(idle, ['status'])).toBe(false)
    expect(hasListFilters({ ...idle, status: 'enabled' }, ['status'])).toBe(
      true
    )
    expect(hasListFilters({ ...idle, q: 'qa' }, [])).toBe(true)
  })
})

describe('page window', () => {
  it('collapses long ranges around the current page', () => {
    expect(pageWindow(6, 12)).toEqual([
      1,
      'start-ellipsis',
      5,
      6,
      7,
      'end-ellipsis',
      12,
    ])
  })

  it('shows every page instead of an ellipsis that would hide a single page', () => {
    expect(pageWindow(3, 5)).toEqual([1, 2, 3, 4, 5])
    expect(pageWindow(1, 1)).toEqual([1])
    expect(pageWindow(9, 3)).toEqual([1, 2, 3])
  })
})

describe('local paging', () => {
  it('slices a complete snapshot and keeps one page for empty results', () => {
    const items = Array.from({ length: 45 }, (_, index) => index)
    expect(pageLocally(items, 3, 20)).toEqual({
      list: [40, 41, 42, 43, 44],
      total: 45,
      page: 3,
      page_size: 20,
      total_pages: 3,
    })
    expect(pageLocally([], 1, 20).total_pages).toBe(1)
  })
})

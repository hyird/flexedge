/** Shortest time a first-load loading state (skeleton, loading placeholder)
 * stays on screen. A faster answer would otherwise flash the skeleton for a
 * few frames before the content replaces it. */
export const minimumLoadingMs = 500

/** Holds whose reveal falls within one frame of each other reveal together. */
const revealTogetherMs = 16

/** A first load starting this long after the epoch's last load started or
 * revealed is unrelated to it (a sheet or popover opened later). Covers the
 * render and effects between a reveal and the reads it mounts. */
const epochQuietMs = 300

/** The loading sequence the user started with one switch: a route change or
 * a tab switch (`begin`). Loads of one page or tab often start in steps — a
 * detail header first, the tab's list once the header arrived — and their
 * placeholders have effectively been on screen since the switch, so every
 * first load of the sequence counts its minimum from the epoch start: the
 * page reveals once the switch is `minimumLoadingMs` old (or as soon as later
 * data arrives), not `minimumLoadingMs` after each step. The sequence
 * continues while its loads keep starting or revealing; a first load after
 * it went quiet starts a new implicit epoch. */
export class LoadingEpoch {
  private start = -Infinity
  private lastActivity = -Infinity

  /** A route change or tab switch: loads that follow count from now. */
  begin(now = Date.now()) {
    this.start = now
    this.lastActivity = now
  }

  /** Start time for a first load whose view shows its loading state since
   * `since`: the epoch start while the sequence continues (or `since` when
   * the view showed it even earlier), otherwise `since`, opening a new
   * epoch. */
  join(since: number) {
    const now = Date.now()
    if (Math.min(since, now) > this.lastActivity + epochQuietMs)
      this.start = since
    this.lastActivity = Math.max(this.lastActivity, now)
    return Math.min(this.start, since)
  }

  /** A first load of the sequence revealed its outcome. */
  settle() {
    this.lastActivity = Math.max(this.lastActivity, Date.now())
  }
}

/** The console's epoch: route changes (`main.tsx`) and tab switches
 * (`TabPanelHold`) begin it; live-query, live logs and the website dashboard
 * time their first loads with it. */
export const loadingEpoch = new LoadingEpoch()

type Hold = { revealAt: number; reveal?: () => void }

/** Keeps first loads in their loading state for at least `ms` after they
 * started (with an `epoch`: after their loading sequence started), then
 * reveals their outcome (data, empty result or error) once.
 *
 * `start(owner)` marks a first load whose view shows a loading state. Each
 * outcome passes through `reveal(owner, show)`: without a hold (cached data,
 * live updates, refetches of loaded data) `show` runs at once; while held,
 * the newest outcome waits until the minimum passed. Outcomes due within one
 * frame reveal in the same task, inside `batch`, so loads that started
 * together (one page's queries) render together. One timer at a time;
 * `cancel` / `clear` drop waiting outcomes and their timer. */
export class MinimumLoading<Owner = object> {
  private holds = new Map<Owner, Hold>()
  private timer: ReturnType<typeof setTimeout> | undefined

  constructor(
    private ms = minimumLoadingMs,
    private batch: (reveal: () => void) => void = (reveal) => reveal(),
    private epoch?: LoadingEpoch
  ) {}

  /** The owner's first load started; its loading state shows since `since`
   * (default now). Ignored while the owner is already held, and for a
   * minimum of 0. A load whose state already showed long enough reveals at
   * once but still counts as part of its epoch. */
  start(owner: Owner, since = Date.now()) {
    if (this.ms <= 0 || this.holds.has(owner)) return
    const from = this.epoch ? this.epoch.join(since) : since
    this.holds.set(owner, { revealAt: from + this.ms })
  }

  /** Shows an outcome of the owner's load now, or once its minimum passed. */
  reveal(owner: Owner, show: () => void) {
    const hold = this.holds.get(owner)
    if (hold && hold.revealAt > Date.now()) {
      hold.reveal = show
      this.schedule()
      return
    }
    if (hold) {
      this.cancel(owner)
      this.epoch?.settle()
    }
    show()
  }

  /** Drops the owner's hold and any outcome still waiting (unmount, abort). */
  cancel(owner: Owner) {
    if (this.holds.delete(owner)) this.schedule()
  }

  clear() {
    this.holds.clear()
    this.schedule()
  }

  private schedule() {
    clearTimeout(this.timer)
    this.timer = undefined
    let next = Infinity
    for (const hold of this.holds.values())
      if (hold.reveal && hold.revealAt < next) next = hold.revealAt
    if (next !== Infinity)
      this.timer = setTimeout(() => this.flush(), next - Date.now())
  }

  private flush() {
    this.timer = undefined
    const now = Date.now()
    const due = [...this.holds].filter(
      ([, hold]) => hold.reveal && hold.revealAt <= now + revealTogetherMs
    )
    const latest = Math.max(...due.map(([, hold]) => hold.revealAt))
    if (latest > now) {
      this.timer = setTimeout(() => this.flush(), latest - now)
      return
    }
    for (const [owner] of due) this.holds.delete(owner)
    this.epoch?.settle()
    this.batch(() => {
      for (const [, hold] of due) hold.reveal?.()
    })
    this.schedule()
  }
}

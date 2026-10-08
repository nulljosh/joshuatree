# Soul

The two ideas under every decision here, and where each one shows up in the software. Joshua set them on 2026-10-07. They are not slogans for a landing page; they are the tie-breaker when two designs both work.

## The pro and the rebel

Steve Jobs was both at once: the professional who shipped finished things, and the rebel who refused the way things were done. One without the other is either a bureaucrat or a kid with a spray can. Joshua Tree keeps both.

**The pro.** Finished means finished.

- Every feature ships with a test that fails without it. Nothing merges on red.
- The docs stay at 100 percent: every source file has a row in `ARCHITECTURE.md`.
- The boot log shows only what matters. Chatter goes to the serial line.
- Type is anti-aliased, icons sit on one grid with one light, and a wrong pixel is a bug.
- A failure says what it is in plain words, with the line that proves it.

**The rebel.** No Linux underneath, no libc, no vendor runtime, no telemetry.

- The whole machine is readable by one person, top to bottom.
- It runs on a board that costs less than a dinner, and the OS stays free forever.
- Claude lives inside it and can rebuild it from the inside. No other OS invites that.
- The logo is a marker scribble, not a corporate mark.
- When the industry says "you need a cloud for that", the answer is a 595 KB firmware upload done by hand over a serial line.

## Seduce, ignore, vilify

Jobs's marketing rule, used here as a design rule.

**Seduce.** The first sixty seconds have to be beautiful. The wallpaper, the dock, the window frame and the type specimen are the first things on screen, before any log line. The landing page boots the real OS in the browser with one click. Samantha speaks.

**Ignore.** Do not answer every feature list. There is no app store, no account to make, no notification centre, no settings maze. The roadmap says no to more things than it says yes to, and the Top 10 is the whole plan. Feature requests that do not serve the demo wait.

**Vilify.** Name the thing we are against, not a competitor's name. The enemy is the computer you cannot read: the OS that is a hundred million lines nobody owns, the chip that needs a cloud login, the settings page with forty toggles. Every "we do not do that" in `DESIGN.md` is a small act of this.

## Aspen, 1983

In 1983 Jobs gave a talk at the Aspen design conference. Four of its points hold up here. The quotes are short phrases from the talk's auto captions, so treat them as close, not exact.

**No magic, only layers.** He said computers are "really dumb", "exceptionally simple but they're really fast". Speed and layers of abstraction make them look like magic. Joshua Tree is built from the bottom up so every layer can be opened and explained. Nothing here should feel like a trick.

**Liberal arts in the machine.** He described Apple as "injecting some liberal arts into these computers": proportional fonts, many fonts, pictures. That is the job `DESIGN.md` does. Type, icons and colour are not decoration; they are the point.

**Closer to artists.** He said computer people are "a lot closer to artists" than the nerd picture suggests. That is the rebel half above, and why we steal like Picasso and not like a committee.

**Voice is the hard part.** He said "understanding language is much harder than understanding voice", because meaning depends on context. That is still true of Samantha. Turning sound into words is the easy part. Knowing what you meant is not, and we should say so plainly instead of pretending.

Steve Jobs, Aspen, 1983.

## How it is enforced

The pro half is enforced by the checks (`tools/checks/`), the 100 percent docs rule and the merge-on-green rule. The rebel half is enforced by what the repo refuses to contain: no vendored kernel, no libc, no analytics, no third-party runtime. `DESIGN.md` holds the never-do list; this page holds the why.

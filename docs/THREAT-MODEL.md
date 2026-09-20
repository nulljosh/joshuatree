# Threat model: Joshua Tree user accounts

Written before the auth code, per roadmap.md's own gate: a login prompt on
top of no real privilege boundary would protect nothing, so this has to be
honest about what "protects" means here before a line of it gets built.

## What a Joshua Tree account actually protects against

The one real scenario this kernel runs in today: a single machine (real
hardware, or QEMU/v86 in a browser tab) with one keyboard and one screen,
used by one person at a time who steps away and comes back. A login screen
protects against a casual second person sitting down at that keyboard
while the first person is away, and it protects against someone opening
the desktop by accident (a stray click, a demo left running) and seeing or
changing another account's files. It also gives this kernel a real,
non-trivial password hash for the first time (salted, iterated SHA-256
instead of nothing), which is worth having on its own, so that adding a
second real account later (multi-user on one machine, still not networked)
doesn't start from a worse place than "no hash at all." That is the whole
scope: a lock on the desktop for the person physically in front of it,
nothing more.

## What it explicitly does NOT protect against

Everything past the keyboard is out of scope and this must not be
oversold as covering it. There is no network-facing multi-tenant use case
today; nothing here is an authentication boundary for a remote caller,
because there is no remote caller. There is no disk encryption: `USERS.TXT`
and every other file live in plaintext on the same FAT image
(`dotfiles.img`) as everything else, so anyone who can read that file off
the disk (copy it to another machine, mount it read-only, boot a different
OS from it) can read every stored salt and hash directly, and could run an
offline crack against the hash exactly as if the login screen never
existed. `USERS.TXT` is not sealed to the login flow that writes it. Most
importantly, this kernel has no ring-3/ring-0 isolation for user data: as
the roadmap already logs, every task today shares one address space and
runs with full ring-0 access to all of memory and the raw disk, so any
code already running inside the kernel (a shell command, a buggy app, a
future third-party binary loaded by `exec`) can read a logged-in session's
memory, or the disk's plaintext `USERS.TXT`, directly, no privilege check
in the way. A login screen does not create that boundary; it was never
going to, and pretending otherwise here would be the "false sense of
security" the roadmap entry warned against.

## What this means for the design

Because of the above, the actual engineering bar is: do the hashing for
real (real salt, real iteration, real constant-time comparison, because a
weak or comparison-timing-leaky hash is a cheap, avoidable mistake even in
a low-stakes threat model), but do not spend effort implying a stronger
boundary exists than does. No claim anywhere in the UI or code comments
that this "secures your data," "encrypts your files," or is safe against
anyone with ring-0 code execution or raw access to the disk image. The
honest framing, and the one this implementation uses: a desktop lock for
the person in front of the machine, backed by a real password hash instead
of a fake one, nothing else.

## The gate is opt-in, not on by default

The login screen only engages once at least one account actually exists
in `USERS.TXT`; an unconfigured system (no `USERS.TXT`, or an empty one)
boots straight to the desktop, exactly as it did before this feature
existed. That is deliberate, not a gap in the implementation, and follows
directly from the framing above: with no account configured there is
nothing for a login screen to protect, so a gate that blocked boot anyway
would be exactly the false sense of security this document already warns
against, a lock on a door with no one behind it. It also matches this
kernel's one real deployment that a login prompt cannot be sprung on:
`landing/index.html`'s live v86 demo boots this exact `kernel.elf` for an
anonymous browser visitor with no keyboard focus guaranteed and no one
able to create an account first, so that build is, and stays,
unconfigured by design. Creating the first account is a deliberate,
in-desktop action (Settings' "Add user" row) rather than something the
boot path forces on every visitor.

# Samantha reset after her face loads (release blocker 2.0.0), postmortem

Status: FIXED. Root cause was the landing demo's kiosk idle timer, not the kernel.

The demo page reboots the guest after 15 s without a mouse move, click or key (`landing/v86/embed.js`). Samantha's 72 face fetches plus a spoken reply ride `/api/proxy` and can outlast that window, so the guest looked like it reset right after her face loaded: the page was restarting it. The fix bumps the idle clock on every `/api/proxy` fetch (the guest fetching is an app at work). `facespeak-demo-check` also asserts exactly one `kmain` banner before the face line.

How it was found: the serial log showed a second `=== kmain boot start ===` with no exception, so no CPU fault at all, which is why QEMU never showed one. A stack overflow into her page directory was the first suspect and was ruled out (her kernel stack sat nowhere near her directory).

The hunt did turn up real kernel hardening that shipped with the fix: 32 KB ring-3 kernel stacks with a guard pad, a stack-room check in `sys_http_get`/`sys_http_post`, and the WIP serial tracing removed. The native repro stays in `samantha-reset-repro.py`.

Separate and pre-existing at the time, not this bug: `speak: status=403`, the speak POST never reached the check's intercept.

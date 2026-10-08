# CI and deploy

How a change gets from a branch to joshuatree.heyitsmejosh.com, on one page.

## The pipeline today

1. **Verify locally.** Every PR opens as a draft, and a draft runs no CI. You run `tools/ci-local.sh` on the Mac instead. It builds `kernel.elf`, runs all four shards of `tools/checks/ci-suite.sh` in parallel QEMUs, then the check-refs scripts and the browser demo checks. It mirrors `.github/workflows/check.yml` job for job, except the `network` job. Only one suite runs at a time (`tools/ci-lock.sh`).
2. **Flip the PR ready.** `gh pr ready <N>` starts `.github/workflows/check.yml` (pull requests only, never on push). Its jobs:
   - `changes` decides what the PR touches. A prose-only PR skips the QEMU suite.
   - `suite` runs the four shards of `ci-suite.sh`. `demo` runs the Playwright landing checks.
   - `check` passes only if `changes`, `suite` and `demo` passed or were rightly skipped.
   - `check-refs` checks doc paths, the version sync and the VERSION bump. It also uploads a Cloudflare version without deploying it, so a bad `wrangler.toml` fails here and not on main. Fork PRs get no token and skip that upload.
   - `network` talks to real internet hosts. It is `continue-on-error`, so it reports but never blocks.
   - `ci` passes only if `check` and `check-refs` both passed.
3. **The gate.** Branch protection on `main` requires `check`, `check-refs` and `network`, with the branch strictly up to date and no reviews. `network` can never fail, so in practice the gate is `check` plus `check-refs`. The `ci` job says the same thing in one place, but it is not on the required list.
4. **Deploy.** Merging pushes to `main`. `.github/workflows/deploy.yml` runs on any push to `main` that touches `landing/**`, `wrangler.toml`, `worker.js`, the kernel sources or a few generators. It builds `kernel.elf` fresh, refreshes the landing facts, headline and roadmap card, and runs `wrangler deploy`. Deploys queue and never cancel each other.
5. **Release.** A push that changes `VERSION` cuts a GitHub release through `.github/workflows/release.yml`.
6. **Smoke the live site.** `.github/workflows/live-smoke.yml` loads the live page and checks that `/api/proxy` refuses a host off its allowlist. It is manual (`workflow_dispatch`). Run it after a deploy finishes. It does not run on push, because nothing guarantees the deploy is finished first.

## Gaps against "CI/CD Made Simple" (Traversy Media)

The video's outline: one verify script, a deploy gate, enforced guardrails, a test that breaks a check on purpose, and a preview per PR. We have the first three. These four are missing.

1. **A preview of the landing page per PR.** Not added, on purpose. A Worker preview version is a version of the same `joshuatree` Worker, and every version gets the Worker's secrets and bindings. A PR preview would run unmerged `worker.js` against the real mail key and mail token, the live waitlist KV, Workers AI billing and the shared rate limiters, at a public URL. Keeping forks out stops a stranger seeing the secrets. It does not stop half-finished code using them. It would also need the kernel build and the three generator steps from `deploy.yml`, or the demo shows nothing. The safe shape is a separate preview Worker with its own KV and no mail or AI secrets, or preview URLs behind Cloudflare Access. Also unverified: whether the config-check versions `check-refs` already uploads get a public preview URL. That depends on the `preview_urls` and `workers_dev` defaults for a Worker on a custom domain. Read one `check-refs` log for a "Version Preview URL" line. If one is there, set `preview_urls = false` in `wrangler.toml`.
2. **A deliberate gate test.** Nothing proves that a red check blocks a merge. The gate already failed silently once: in 1.5.14, a shell line let a failed shard pass `check`. A test would open a throwaway PR that fails one check on purpose, then confirm `gh pr merge` is refused. It could also assert the required-check list through the API, so `ci` or `check` cannot drop off unnoticed.
3. **Deploy only after the checks are green on main.** `deploy.yml` fires on every matching push to `main` and reads no check result. Strict up-to-date branch protection means the merged tree is the one that passed. A direct push or an admin merge would still deploy unchecked. The fix: trigger the deploy from a check run on `main` (`workflow_run`), or check the commit's status before `wrangler deploy`.
4. **Rollback.** There is no rollback step. Today you revert the commit and wait for a fresh deploy. `wrangler rollback` returns to the previous version in seconds. It could be a manual workflow, and the natural place to trigger it is a red `live-smoke.yml`.

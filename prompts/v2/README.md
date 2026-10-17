# v2 library build — run order

Base branch for everything: `next`. Every task branches from `next` and opens a PR back into `next`.
Nothing goes into `main` until the bench demo passes.

## Two people, two tracks

The work splits into a firmware track and a host track that don't touch the same folders,
so two people (each on their own Claude plan) can run them at the same time.

```
            ┌─ Person A (firmware) ─  L1 link-esp ──▶ L3 node ──┐
schema ─────┤                                                   ├──▶ L5 ota-diag ──▶ bench demo ──▶ main
 (PR #11)   └─ Person B (host) ─────  L2 link-host ─▶ L4 host ──┘
```

| Step | Who | Prompt | Branch | Start after |
|------|-----|--------|--------|-------------|
| 0 | either | merge PR #11 `lib/schema` | — | its CI is green |
| 1 | A | `L1-link-esp.md` | `lib/link-esp` | step 0 |
| 2 | B | `L2-link-host.md` | `lib/link-host` | step 0 |
| 3 | A | `L3-node.md` | `lib/node` | L1 merged |
| 4 | B | `L4-host.md` | `lib/host` | L2 merged |
| 5 | whoever is free | `L5-ota-diag.md` | `lib/ota-diag` | L3 **and** L4 merged |
| 6 | both, on the bench | demo checklist below | — | L5 merged |
| 7 | either | merge `next` into `main` | — | demo passes |

Don't run two sessions at once on the same plan: it burns that plan's usage twice as fast.
One session per person at a time.

## How to start each session

Open a new Claude Code session on this repo and paste (change the file name):

> Check out `next` and pull. Do the task in `prompts/v2/L1-link-esp.md`. Follow CLAUDE.md.
> Work on the branch named in the prompt, open a PR into `next`, then stop.

When the PR's CI is green, merge it on GitHub, then start your next step.

If a session hits a usage limit mid-task, wait for the reset, then in a new session say:
"Continue the task in `prompts/v2/<file>` on branch `<branch>`. Read what's already pushed first."

## Avoiding conflicts

- Each track stays in its own folders (firmware vs host). The shared files most likely to clash are
  `.github/workflows/ci.yml`, `README.md` and `CLAUDE.md`. Whoever merges second: before merging,
  tell your session "Merge `next` into this branch, fix any conflicts, push" and wait for CI.
- Never edit `schema/` in a track PR. If the schema needs a change, do it as its own small PR into
  `next`, merge it, and tell the other person to pull.
- Merge PRs promptly so the other track builds on the latest `next`.

## Step 6 — bench demo checklist

On the RPi bench with at least two ESP32s running the new node library:

- [ ] Both nodes get IDs automatically and are listed by the host tool
- [ ] Each node reports its type and the host matches it to the schema (version check passes)
- [ ] Read and change a setting live; it survives a node reboot
- [ ] Change a stream rate live and see telemetry rate change
- [ ] Arm, send a command, stop sending: node disarms within the watchdog timeout
- [ ] E-stop disarms every node immediately
- [ ] Reflash one node over CAN; a deliberately broken image rolls back
- [ ] An error event raised on a node shows up on the host

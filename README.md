# Anveshak rover CAN system

Spec: `docs/architecture.md`. Rules for Claude sessions: `CLAUDE.md`. Build tasks: `prompts/`.

## Who does what

The work is split between two people so you never edit the same folders:

- **Person A: firmware** (`firmware/`), plus the shared definitions (`defs/`) at the start.
- **Person B: host side** (`host/`, `dashboard/`).

The DBC file in `defs/` is the contract between you. The simulator (built in P0) pretends to be
the nodes, so person B never has to wait for person A's firmware.

## Order of work

**Tier 1, must-have.** The rover can be driven over CAN from one host, and you can see every node
on a dashboard.

| Step | Who | Prompt | Needs first | Rough cost |
|---|---|---|---|---|
| 1 | A | `P0-definitions-and-simulator` | nothing | $8–12 |
| 2 | A | `A1-firmware-core` | P0 merged | $15–25 |
| 2 | B | `B1-gateway-core` | P0 merged | $15–25 |
| 3 | A | `A2-drivers-and-drive-node` | A1 merged | $10–20 |
| 3 | B | `B2-ros2-package` | B1 merged | $8–15 |
| 4 | B | `B3-dashboard-mvp` | B1 merged (B2 not needed) | $15–25 |

Steps with the same number run at the same time, one per person.

**Tier 2.** Reflashing over CAN, debugging and error reports, more dashboard controls.

| Who | Prompt | Needs first | Rough cost |
|---|---|---|---|
| A | `A3-bootloader-and-flasher` | A2 | $10–20 |
| A | `A4-node-diagnostics-and-debug` | A2 | $10–15 |
| B | `B4-gateway-link-debug-and-errors` | B3 | $12–20 |
| B | `B5-control-panel` | B3 | $12–20 |

**Tier 3.** Nice to have.

| Who | Prompt | Needs first | Rough cost |
|---|---|---|---|
| A | `A5-esp32-ota` | A4 | $6–10 |
| B | `B6-frame-lab-profiles-reports` | B4, B5 | $12–20 |

Costs are rough estimates, mostly using Sonnet. Tier 1 is about $70–120 for both of you
together.

## One-time setup (one person, about 15 minutes)

1. Create a GitHub repo, for example `anveshak-can` (private is fine). Add your friend as a
   collaborator (Settings → Collaborators).
2. Copy everything in this starter pack into the repo, then commit and push to `main`.

## Each person, once

1. Open https://claude.ai/code and sign in.
2. Connect GitHub when it asks: install the **Claude GitHub App** and give it access to the
   `anveshak-can` repo.
3. Keep the default cloud environment (Trusted network access) for now.
4. In claude.ai **Settings → Usage**, check that pay-as-you-go **usage credits are off**. Then
   nothing can ever cost money: after the free cloud credit runs out, work just continues on your
   normal plan limits.
5. Look at the **Cloud session credits** meter on the same Settings page; that's your $100.

## Running one step

1. At claude.ai/code, start a **new session**: pick the repo, branch `main`.
2. Type `/model sonnet` and send it. Sonnet is much cheaper than Opus and fine for this work.
3. Send this message (change the prompt name):

   > Do the task in `prompts/A1-firmware-core.md`. Follow `CLAUDE.md`. Work on a new branch and open a PR when done.

4. Leave it running. It usually takes 30 minutes to 2 hours. You can close the browser; it keeps
   going. If it asks a question, answer it from the session page or the Claude phone app.
5. When it finishes, review the changes, open the PR, wait for CI to pass, and merge into `main`.
6. Check the credit meter. Start the next step in a **new** session (fresh sessions cost less
   than one long one).

Rules that save money:

- One session at a time per person.
- Don't start an extra session to "explore" or "improve" things.
- If a session goes in the wrong direction, stop it early and start again with a clearer message,
  rather than letting it dig deeper.
- Only switch to Opus (`/model opus`) if Sonnet is clearly stuck on a hard part, for example the
  bootloader.
- When your credit is nearly used up, finish the current step and do the rest on your normal plan
  limits, which are slower but free.

## When something needs a change in `defs/`

Make it a small separate PR first, tell the other person, and merge it before continuing. Never
change a message layout only in code.

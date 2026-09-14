# A design that ships as a document is NOT done

**Operator ruling, 2026-09-14. This applies to every KTP repo.**

## The rule

When a PR lands whose product is a **design** — a proposal, an assessment, an audit, a scoping doc,
a runbook for work not yet done, a handover — **every proposal inside it becomes a tracked item in
the same act**, before the session moves on.

Not a summary in chat. Not "it's in the doc". An item on the board, under a card, that an agent
could be spawned against tomorrow.

## Where it came from

`searse/keep-the-prac`#720 shipped a navigation proposal on 2026-09-14. It merged. Four of its
recommendations were never built, and nobody noticed until the operator asked:

> "I thought we were implementing those changes, but instead we produced a design document that got
> lost."

The only part that shipped was an incidental bug the proposal found on the way — because that bug
was filed as an item and the proposals were not. Hours later the same session repeated the defect
with four more audit PRs.

## The mechanical test

Open the document. List its proposals. Account for **every one**:

- **built** — name the PR or commit;
- **filed** — one board item, one line, pointing at the document;
- **a finding, not work** — record it on the card and file nothing.

A proposal accounted for in none of those three ways is the defect this rule exists to catch.

## What is NOT an item

⛔ **A finding is not a proposal.** #720's second recommendation was "no route is redundant enough to
remove" — a conclusion with nothing to build. Filing findings as work inflates the board and hides
what remains.

⛔ **The reasoning is not the item.** One line: the verdict and the pointer. The evidence stays in
the PR; the durable lesson goes to `CLAUDE.md`, a repo doc, or memory.

## Two traps that produced this

⚠️ **"The operator will read the doc" is how #720 was lost.** A proposal written as a decision
request needs the decision put to the operator **as a question**, plus an item saying what gets built
once he answers. A document that waits to be read is a document that waits forever.

⚠️ **An agent's board note is not filing.** An agent that cannot hold the pen writes a note; the
pen-holder owes the items. A note read and not routed is the same loss with an extra step.

## Auditing a repo in one pass

```bash
gh pr list --repo <owner>/<repo> --state merged --limit 200 --json number,title,files
# keep the PRs whose files are ALL docs, then for each:
#   open its document, list its proposals
#   grep TODO.md for the PR number
```

**Zero board mentions on a proposal PR is the alarm.**

---

*Full rule: `TODO_PROCESS.md` § 2b in the KTP doc set. Filing threshold (card vs inline item vs not
filed at all): § 2a.*

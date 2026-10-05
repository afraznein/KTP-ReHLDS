#!/bin/sh
# Builds a throwaway repo plus a linked worktree and proves the commit-sweep guard.
# HOOK=<file> tests another hook (a no-op there is the negative control).
set -u
here=$(cd "$(dirname "$0")" && pwd -P)
HOOK=${HOOK:-$here/pre-commit}
tmp=$(mktemp -d)
tmp=$(cd "$tmp" && { pwd -W 2>/dev/null || pwd; })   # a native path, so git.exe needs no MSYS path conversion
trap 'rm -rf "$tmp"' EXIT
fails=0

check() { # check <name> <want: pass|block> <cmd...>
  name=$1; want=$2; shift 2
  if "$@" >"$tmp/out" 2>&1; then got=pass; else got=block; fi
  if [ "$got" = "$want" ]; then echo "ok   $name ($got)"; else echo "FAIL $name (want $want, got $got)"; sed 's/^/     /' "$tmp/out"; fails=$((fails + 1)); fi
}

g() { git -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false "$@"; }

n=0
dirty() { # dirty <tree>: both files modified, nothing staged
  n=$((n + 1)); echo "$n" >"$1/mine.txt"; echo "$n" >"$1/theirs.txt"; git -C "$1" reset -q
}

main=$tmp/main
git init -q "$main"
mkdir "$main/.githooks"
cp "$HOOK" "$main/.githooks/pre-commit"
chmod +x "$main/.githooks/pre-commit"
echo a >"$main/mine.txt"; echo a >"$main/theirs.txt"
g -C "$main" add -A
g -C "$main" commit -q -m init
git -C "$main" config core.hooksPath .githooks

dirty "$main"
git -C "$main" add theirs.txt mine.txt
check "main: bare commit with another session's staging" block g -C "$main" commit -q -m sweep
dirty "$main"
check "main: commit -a" block g -C "$main" commit -q -a -m sweep

dirty "$main"
git -C "$main" add theirs.txt
check "main: pathspec commit" pass g -C "$main" commit -q -m mine -- mine.txt
check "main: pathspec commit took only its path" pass test "$(git -C "$main" show --name-only --format= HEAD)" = mine.txt
check "main: other session's staging left staged" pass test "$(git -C "$main" diff --cached --name-only)" = theirs.txt

dirty "$main"
git -C "$main" add -A
check "main: override" pass env KTP_COMMIT_SWEEP=1 git -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false -C "$main" commit -q -m override
git -C "$main" reset -q --hard
check "main: nothing staged" pass g -C "$main" commit -q --allow-empty -m empty

g -C "$main" worktree add -q -b side "$tmp/wt" >/dev/null 2>&1
dirty "$tmp/wt"
git -C "$tmp/wt" add -A
check "linked worktree: bare commit" pass g -C "$tmp/wt" commit -q -m wt
dirty "$tmp/wt"
check "linked worktree: commit -a" pass g -C "$tmp/wt" commit -q -a -m wt2

printf '#!/bin/sh\nexit 1\n' >"$main/.git/hooks/pre-commit"
chmod +x "$main/.git/hooks/pre-commit"
dirty "$main"
check "main: a failing .git/hooks/pre-commit still runs" block g -C "$main" commit -q -m chained -- mine.txt
rm "$main/.git/hooks/pre-commit"

if [ -f "$here/pre-push" ]; then
  cp "$here/pre-push" "$main/.githooks/pre-push"
  chmod +x "$main/.githooks/pre-push"
  git init -q --bare "$tmp/remote.git"
  git -C "$main" remote add origin "$tmp/remote.git"
  check "pre-push: no gate installed" pass git -C "$main" push -q origin HEAD:refs/heads/a
  printf '#!/bin/sh
exit 1
' >"$main/.git/hooks/pre-push"
  chmod +x "$main/.git/hooks/pre-push"
  check "pre-push: an installed .git/hooks/pre-push still runs" block git -C "$main" push -q origin HEAD:refs/heads/b
  rm "$main/.git/hooks/pre-push"
fi

[ "$fails" -eq 0 ] && echo "all passed" || echo "$fails failed"
[ "$fails" -eq 0 ]

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check-docs.py -- verify that file paths mentioned in Markdown really exist.

Why: the docs are a main deliverable of the format analysis work, and a
stale path (`src/foo.c` that was renamed, `docs/bar.md` that was never written)
makes them useless.  This script scans every Markdown file that ships in the
repository, extracts path-like tokens (inline code, plain text, fenced blocks),
and checks each token that points *into this repository*.

Classification
--------------
* in repository -- resolves to an existing file/directory under the repo root.
* outside repo  -- points outside the shipped tree on purpose (`work/`, `out/`,
                   `extract/`, `bin/`, `tools-src/`, the game directory, the
                   external `fvp-android` project, ...).  Informational only.
* MISSING       -- a repository path that does not exist.  This fails the
                   check.  Only tokens with a file extension are treated as
                   required (directories/globs are prose unless they resolve).
* skipped       -- prose, URLs, absolute Windows paths, version numbers, bare
                   file names that do not resolve, ...

Resolution follows Markdown semantics: relative to the Markdown file first, then
to the repository root, then to the repository's parent directory (the docs were
written in a workspace whose root contains `fvp/`, so `fvp/src/vm.c` is accepted
as well).  Glob tokens with `*` are matched against the shipped file list.

Usage
-----
    python scripts/check-docs.py                 # summary + missing paths
    python scripts/check-docs.py --verbose       # also list outside-repo refs
    python scripts/check-docs.py --show-skipped  # also list unchecked prose
    python scripts/check-docs.py --root DIR      # check another checkout

Exit codes: 0 = no missing repository path, 1 = missing path(s),
2 = repository layout looks wrong (no Markdown found).
"""
from __future__ import annotations

import argparse
import fnmatch
import io
import os
import re
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

#: Folders scanned at the top level (docs may live in the root or in docs/).
DOC_DIRS = ('.', 'docs')

#: Top-level directories that are part of the shipped repository.
REPO_TOPS = ('src', 'tests', 'docs', 'ref', 'scripts', '.github')

#: Root-level files that people reference by bare name.  These are the only
#: bare names that fail the check when absent (a bare `device-test.ps1` may
#: belong to an external project, but `LICENSE` must exist here).
ROOT_NAMES = ('LICENSE', 'VERSION', 'Makefile', '.gitignore', 'README.md',
              'README.en.md', 'CONTRIBUTING.md', 'THIRD-PARTY.md')

#: Extensions that identify a *file* (used to decide "missing" vs "prose").
FILE_EXTS = frozenset((
    '.md', '.c', '.h', '.hh', '.hpp', '.cc', '.cpp', '.py', '.ps1', '.sh',
    '.txt', '.json', '.yml', '.yaml', '.toml', '.cfg', '.ini', '.csv', '.tsv',
    '.html', '.css', '.js', '.java', '.kt', '.gradle', '.xml', '.rs', '.asm',
    '.hcb', '.bin', '.dat', '.png', '.jpg', '.ogg', '.wav', '.exe', '.dll',
    '.a', '.o', '.zip', '.7z', '.apk', '.log',
))

#: Directories never scanned for Markdown (build output, local work areas,
#: third-party vendored docs, VCS metadata).
SKIP_DIRS = frozenset((
    '.git', '.hg', '.svn', '__pycache__', 'node_modules', 'out', 'extract',
    'bin', 'work', 'tools-src', 'dist', '.venv', 'venv', '.idea', '.vscode',
))

#: Directories skipped when building the file index.  Note that this set is
#: *smaller*: bare file names such as `hzc1.py` still resolve to the local,
#: unshipped `work/` copy so they are reported as "outside repo" instead of
#: "missing" (the docs reference those prototypes by name).
INDEX_SKIP_DIRS = frozenset((
    '.git', '.hg', '.svn', '__pycache__', 'node_modules', 'dist',
    '.venv', 'venv', '.idea', '.vscode',
))

#: A path-like token.  '*' is included so globs such as src/gen_*.h stay whole.
TOKEN_RE = re.compile(r'[A-Za-z0-9_@$*./\\-]+')

#: Punctuation that can hug a path in prose or in Markdown.
TRIM = '.,;:)]}>\'"'

# 每份交付文档顶部都要声明「由 DeepSeek 生成」。中英两种写法都认。
NOTICE_MARKERS = (
    '不一定代表作者个人意愿',
    "may not represent the author's personal views",
)


class Stats(object):
    def __init__(self):
        self.files = 0
        self.refs = 0
        self.ok = 0
        self.outside = 0
        self.skipped = 0
        self.missing = 0
        self.missing_detail = []   # (md file, line, token)
        self.outside_detail = []   # (md file, line, token)
        self.skipped_detail = []   # (md file, line, token, reason)


def repo_root_from_script():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


def markdown_files(root):
    found = []
    seen = set()
    for rel in DOC_DIRS:
        base = root if rel == '.' else os.path.join(root, rel)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            path = os.path.join(base, name)
            if name.lower().endswith('.md') and os.path.isfile(path):
                found.append(os.path.abspath(path))
                seen.add(os.path.abspath(path))
    # Pick up Markdown anywhere else (future docs/<sub>/foo.md).
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            if not name.lower().endswith('.md'):
                continue
            path = os.path.abspath(os.path.join(dirpath, name))
            if path not in seen:
                found.append(path)
                seen.add(path)
    return found


def build_file_index(root):
    """Return (basename -> [repo-relative paths], [all repo-relative paths]).

    The index covers the whole working tree (including unshipped folders such
    as `work/`) so that a bare reference to a local prototype is classified as
    "outside repo" rather than "missing".
    """
    index = {}
    rels = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in INDEX_SKIP_DIRS]
        for name in filenames:
            rel = os.path.relpath(os.path.join(dirpath, name), root)
            rel = rel.replace(os.sep, '/')
            rels.append(rel)
            index.setdefault(name, []).append(rel)
    return index, rels


def is_under(path, base):
    try:
        common = os.path.commonpath([os.path.abspath(path),
                                     os.path.abspath(base)])
    except ValueError:  # different drives on Windows
        return False
    return common == os.path.abspath(base)


def classify_scope(rel_posix):
    """True if a repo-relative path belongs to the shipped tree."""
    return rel_posix.split('/', 1)[0] in REPO_TOPS


def normalize(token):
    tok = token.replace('\\', '/')
    while tok.startswith('./'):
        tok = tok[2:]
    return tok


def ext_of(tok):
    base = tok.rsplit('/', 1)[-1]
    dot = base.rfind('.')
    return base[dot:] if dot > 0 else ''


def has_ext(tok):
    return ext_of(tok) in FILE_EXTS


def repo_scoped(tok):
    """Is this token supposed to point into the shipped repository?"""
    if tok.startswith('../'):
        return True
    if tok.startswith('fvp/'):
        return tok[4:].split('/', 1)[0] in REPO_TOPS
    return tok.split('/', 1)[0] in REPO_TOPS


def glob_matches(tok, all_rels):
    """Expand a glob token against the shipped file list (posix paths)."""
    pattern = tok[4:] if tok.startswith('fvp/') else tok
    hits = [rel for rel in all_rels
            if fnmatch.fnmatch(rel, pattern)
            or fnmatch.fnmatch(rel, './' + pattern)]
    if not hits and tok.startswith('../'):
        hits = [rel for rel in all_rels if fnmatch.fnmatch(rel, pattern)]
    return hits


def resolve_slashed(root, parent, md_dir, tok, index, all_rels):
    """Resolve a token containing a slash.  Returns ('ok'|'outside'|'missing'
    |'prose', detail)."""
    candidates = [os.path.join(md_dir, tok), os.path.join(root, tok)]
    if tok.startswith('fvp/'):
        stripped = tok[4:]
        candidates.insert(0, os.path.join(root, stripped))
        candidates.append(os.path.join(parent, tok))
    candidates.append(os.path.join(parent, tok))

    for cand in candidates:
        if os.path.exists(cand):
            cand = os.path.abspath(cand)
            if is_under(cand, root):
                rel = os.path.relpath(cand, root).replace(os.sep, '/')
                return ('ok' if classify_scope(rel) else 'outside'), rel
            return 'outside', cand

    if '*' in tok:
        hits = glob_matches(tok, all_rels)
        if hits:
            shipped = [h for h in hits if classify_scope(h)]
            if shipped:
                return 'ok', shipped[0]
            return 'outside', hits[0]
        return ('missing' if repo_scoped(tok) and has_ext(tok) else 'prose'), None

    if repo_scoped(tok):
        return ('missing' if has_ext(tok) else 'prose'), None
    return 'outside', None


def resolve_bare(root, md_dir, tok, index):
    """Resolve a token without a slash.

    Bare names are weak evidence: `compare.py` may be prose, and the frozen
    docs also mention helper scripts of the *external* Android project.  So a
    bare name that resolves anywhere is reported (and classified), a bare root
    file name is required, and anything else is treated as prose.
    """
    for cand in (os.path.join(md_dir, tok), os.path.join(root, tok)):
        if os.path.exists(cand):
            cand = os.path.abspath(cand)
            if is_under(cand, root):
                rel = os.path.relpath(cand, root).replace(os.sep, '/')
                return ('ok' if classify_scope(rel) else 'outside'), rel
            return 'outside', cand

    matches = index.get(tok, [])
    if matches:
        shipped = [m for m in matches if classify_scope(m)]
        return ('ok' if shipped else 'outside'), (shipped or matches)[0]

    if tok in ROOT_NAMES:
        return 'missing', None

    return 'prose', None


def scan(root, stats):
    parent = os.path.dirname(os.path.abspath(root))
    files = markdown_files(root)
    stats.files = len(files)
    if not files:
        return
    index, all_rels = build_file_index(root)

    for path in files:
        md_rel = os.path.relpath(path, root).replace(os.sep, '/')
        md_dir = os.path.dirname(path)
        try:
            with open(path, 'r', encoding='utf-8', errors='replace') as fh:
                lines = fh.readlines()
        except OSError as exc:
            print('check-docs: cannot read %s: %s' % (md_rel, exc),
                  file=sys.stderr)
            continue
        for lineno, line in enumerate(lines, 1):
            for match in TOKEN_RE.finditer(line):
                raw = match.group(0)
                tok = raw.strip(TRIM)
                if len(tok) < 3:
                    continue
                if ':' in tok:                       # URL / drive letter / key
                    continue
                if tok.startswith(('/', '~', '-')):
                    continue
                if tok.startswith('...') or re.match(r'^0[xX]', tok):
                    continue
                if '//' in tok:
                    continue
                if tok.endswith('.'):
                    tok = tok.rstrip('.')
                    if not tok:
                        continue
                norm = normalize(tok)
                if not norm:
                    continue
                if '/' in norm:
                    state, detail = resolve_slashed(root, parent, md_dir, norm,
                                                    index, all_rels)
                else:
                    state, detail = resolve_bare(root, md_dir, norm, index)
                if state == 'ok':
                    stats.refs += 1
                    stats.ok += 1
                elif state == 'missing':
                    stats.refs += 1
                    stats.missing += 1
                    stats.missing_detail.append((md_rel, lineno, tok))
                elif state == 'outside':
                    stats.refs += 1
                    stats.outside += 1
                    stats.outside_detail.append((md_rel, lineno, tok))
                else:
                    stats.skipped += 1
                    stats.skipped_detail.append(
                        (md_rel, lineno, tok, 'prose / not a repo path'))


def notice_violations(root):
    """Return Markdown files missing the "generated by DeepSeek" notice.

    Every hand-off document must say, near the top, that it was generated and
    may not represent the author's own views.  `ref/` is exempt: those files are
    frozen data records, not prose.
    """
    missing = []
    for path in markdown_files(root):
        rel = os.path.relpath(path, root).replace('\\', '/')
        if rel.startswith('ref/'):
            continue
        try:
            with io.open(path, 'r', encoding='utf-8', errors='replace') as f:
                head = [next(f, '') for _ in range(20)]
        except OSError:
            continue
        if not any(any(m in line for m in NOTICE_MARKERS) for line in head):
            missing.append(rel)
    return sorted(missing)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Verify that paths referenced by Markdown exist.')
    ap.add_argument('--root', default=None,
                    help='repository root (default: parent of scripts/)')
    ap.add_argument('--verbose', '-v', action='store_true',
                    help='list references that point outside the repository')
    ap.add_argument('--show-skipped', action='store_true',
                    help='also list prose tokens that were not checked')
    ap.add_argument('--no-notice', action='store_true',
                    help='skip the "generated by DeepSeek" notice check')
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root or repo_root_from_script())
    if not os.path.isdir(root):
        print('check-docs: not a directory: %s' % root, file=sys.stderr)
        return 2
    if not os.path.isfile(os.path.join(root, 'README.md')):
        print('check-docs: %s does not look like the repository root '
              '(no README.md)' % root, file=sys.stderr)
        return 2

    stats = Stats()
    scan(root, stats)

    if stats.files == 0:
        print('check-docs: no Markdown files found under %s' % root,
              file=sys.stderr)
        return 2

    print('check-docs: %d Markdown files under %s' % (stats.files, root))
    print('  references checked : %d' % stats.refs)
    print('    in repository    : %d' % stats.ok)
    print('    outside repo     : %d  (work/, out/, game data, external '
          'projects)' % stats.outside)
    print('  prose/other skipped: %d' % stats.skipped)
    print('  missing            : %d' % stats.missing)

    if args.verbose and stats.outside_detail:
        print('\nNot shipped (expected, informational):')
        for md, line, tok in stats.outside_detail:
            print('  %s:%d  %s' % (md, line, tok))
    if args.show_skipped and stats.skipped_detail:
        print('\nNot treated as paths:')
        for md, line, tok, why in stats.skipped_detail:
            print('  %s:%d  %s  (%s)' % (md, line, tok, why))

    if stats.missing:
        print('\nMissing repository paths:')
        for md, line, tok in stats.missing_detail:
            print('  %s:%d  %s' % (md, line, tok))
        print('\nRESULT: FAIL -- %d missing path(s)' % stats.missing)
        return 1

    no_notice = []
    if not args.no_notice:
        no_notice = notice_violations(root)
        print('  notice present     : %d  (ref/ exempt)'
              % (len(markdown_files(root)) - len(no_notice)))
        print('  notice missing     : %d' % len(no_notice))
        if no_notice:
            print('\nMarkdown files without the "generated by DeepSeek" notice '
                  '(expected within the first 20 lines):')
            for rel in no_notice:
                print('  %s' % rel)
            print('\nRESULT: FAIL -- %d file(s) missing the notice'
                  % len(no_notice))
            return 1

    print('\nRESULT: PASS -- 0 missing paths, notice on every hand-off document')
    return 0


if __name__ == '__main__':
    sys.exit(main())
#!/usr/bin/env python3
"""Sweep PORT_NOTES + Source comments for deferral/interim/TODO markers.

The 2026-07-15 PIE findings showed 'deferred (presentation)' notes scatter
across PORT_NOTES and code comments and never roll up — items marked
deferred quietly leave every queue. This emits the raw hit list; the
curated ledger lives at docs/DEFERRED.md (regenerate hits, then re-triage:
each hit is either LIVE (goes in the ledger + a queue) or RESOLVED
(historical text — leave it). Do the triage with the entry's surrounding
context, not the single line.

Usage: python3 Tools/test/deferral_sweep.py [> hits.txt]
"""
import io
import re
import sys
from pathlib import Path

# Windows cp1252 stdout chokes on the docs' arrows/dashes.
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8',
                              errors='replace')

REPO = Path(__file__).resolve().parents[2]
DOC_PAT = re.compile(
    r'deferr|interim|stays? deferred|not yet|TODO|OPEN QUESTION|OBSERVED|'
    r'unported|parked', re.I)
SRC_PAT = re.compile(r'//.*(deferr|interim|TODO|unported)', re.I)
# Systematic false positives found during the 2026-07-15 DEFERRED.md triage:
# engine idioms and identifiers that merely CONTAIN a marker word.
FALSE_PAT = re.compile(
    r'SpawnActorDeferred|deferred[- ]spawn|DeferredDecal|TimeToDoSound', re.I)


def main():
    hits = []
    notes = REPO / 'PORT_NOTES.md'
    for i, line in enumerate(
            notes.read_text(encoding='utf-8', errors='replace').splitlines(), 1):
        if DOC_PAT.search(line) and not FALSE_PAT.search(line):
            hits.append((f'PORT_NOTES.md:{i}', line.strip()[:160]))
    for src in (REPO / 'Source' / 'WoT').rglob('*.[ch]*'):
        if src.suffix not in ('.cpp', '.h'):
            continue
        try:
            txt = src.read_text(encoding='utf-8', errors='replace')
        except OSError:
            continue
        for i, line in enumerate(txt.splitlines(), 1):
            if SRC_PAT.search(line) and not FALSE_PAT.search(line):
                hits.append((f'{src.relative_to(REPO)}:{i}', line.strip()[:160]))
    print(f'{len(hits)} deferral markers')
    for loc, line in hits:
        print(f'{loc}: {line}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

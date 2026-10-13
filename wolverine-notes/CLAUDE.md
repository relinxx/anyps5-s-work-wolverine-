# Claude Code working preferences (Wolverine / AnyPS5 project)

## Usage and cost
- Main model: Opus 5.5. Default reasoning effort: medium. Recommend high effort only for genuinely complex problems (for example hard concurrency, memory-ownership or emulator-correctness debugging) and say why.
- Do not spawn subagents, agent teams, workflows or parallel investigations unless the user explicitly authorizes them in the current request.
- Before an expensive investigation (many file reads, long builds, game runs, broad searches), state the intended approach and estimated scope first.
- Do not install or enable RTK hooks, Serena, Context Mode or any other tool or plugin without the user's approval.

## Logs and source
- Emulator/game logs (stderr.log, stream-events.jsonl, run directories) are often many MB. Never load a whole multi-megabyte log into context. Search for relevant errors, timestamps, markers and patterns first (Select-String/Grep with -Pattern, -Context, Select-Object -First/-Last), then read only the matching sections.
- Read only the relevant source files and line ranges. Do not re-read files that have not changed.
- Reuse existing findings, build results, test logs and investigation notes (work/research/*.txt, claude-review/, handover files) instead of repeating work.

## Responses
- Keep responses concise, but keep the important technical explanations, evidence (file:line, log lines, exit codes) and verification results.

# Instructions for AI assistants

This repository is NX2Harmony: a Windows app and command line that build the HarmonyOS edition of the Switch games in
`games.tsv` from the player's own game files and keys, sign it with the player's own Huawei account and install it on the
player's own phone. Nothing of any game is in the repository. The game pipelines (the backends) are developed in their
own projects; `backends/` holds the copies NX2Harmony builds with (`backends/README.md`).

- **To install, update or rebuild a game**, read `.claude/skills/install-nx2harmony/SKILL.md` and follow it step by step.
  Claude Code loads it by itself (`/install-nx2harmony`); other assistants read the file.
- Never download or ask for game files, keys or firmware; never print `prod.keys`, `build-profile.json5` or anything
  under `%USERPROFILE%\.nx2h`; never weaken a hash check, a backend's package audit or the release gate in
  `packaging/full_bundle.py`; never upload, commit or share anything under `prepared/`, `artifacts/`, `work/` or a built
  package (a HAP or the full package). The skill's Rules section has the full list.
- The full package (the packed app plus the game backends' sources, no game content) comes only from this
  repository's Releases page. Never look for it anywhere else; when one is needed, use the player's copy or that page.
- Ask the player which folder holds their game files before looking; list file names only, never whole drives.
- The player does the account steps (Huawei sign-in in the browser, DevEco Studio, prompts on the phone), answers
  Windows' administrator prompts (winget, WSL 2) and restarts Windows after `setup --fix` installed WSL 2. Back up saves before anything that uninstalls the app.
- Reply in the player's language.

For changes to the code itself: `README.md` describes the layout, `CONTRIBUTING.md` the rules (game-specific work belongs
to the backend projects and is then imported into `backends/` with `packaging/backend_import.py`), `CHANGELOG.md` what
each version changed, and `python -m unittest discover -s tests` runs the checks that need no game files.

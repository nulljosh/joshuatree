# Decisions

Standing answers, so nobody has to ask again. Locked 2026-10-06 night by Joshua. Change one by editing it here, with the date.

1. **Who can push.** The Claude relay needs a token and listens on the local network only. Claude opens pull requests and never merges. CI merges on green, like everything else.
2. **Power cuts.** Keep a spare card with the last good build. A fallback kernel boots the old build if a new one fails. A journaling filesystem comes later.
3. **Who owns what.** The OS stays free forever, under Apache 2.0. The hardware designs are CC BY-NC-SA. The Wi-Fi firmware is not ours: it downloads at flash time and never enters the repo.
4. **When 3.0 is done.** The desktop boots on a real Raspberry Pi 4 and you can type and click on it, mouse included.
5. **Who it is for.** Joshua first. Then the people who build computers for fun and would buy the kit. Kids come later; that is the version 100 idea.
6. **Backups.** GitHub holds the code, the LaCie holds the rest. Push every day.
7. **Usage.** Stop starting new work at 90 percent of a weekly limit. Haiku for small jobs. At most three helpers, two if any is Opus or Fable.

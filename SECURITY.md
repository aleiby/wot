# Security and takedown contact

## Reporting a vulnerability

Please report security problems privately, not in a public issue.

Use GitHub's private vulnerability reporting: open the **Security** tab of this
repository and choose **Report a vulnerability**. Include the version you ran
(the first line of the release notes you installed from), the platform, and the
steps that reproduce the problem.

In scope, in rough order of importance:

- the first-run bootstrap and the in-game updater, including the signed release
  channel (announcements, manifests, key rings) and how the game verifies and
  stages what it downloads;
- the handling of the original game's installer and data on the player's
  machine (the game must never read, write or send anything outside what the
  install notes describe);
- multiplayer networking, both LAN and the internet relay path;
- the build and release tooling under `Tools/dist`.

You should hear back within a week. Fixes ship as a normal release; the release
notes credit reporters who want to be credited.

## Rights holders

This is an unofficial fan project. It redistributes none of the original game:
every player supplies their own purchased copy, and apart from vendored
third-party libraries under their own licenses, the repository contains only
code and documentation written for the project (see [NOTICE](NOTICE)).

If you hold rights to The Wheel of Time (1999), to its publisher's marks or to
the Wheel of Time literary property and believe something here infringes them,
contact the maintainer through the address on the maintainer's GitHub profile,
or use GitHub's DMCA process. The project will respond promptly and remove
anything that should not be here.

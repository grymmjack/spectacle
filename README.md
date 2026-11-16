# Spectacle - The KDE Screenshot Utility

Spectacle is a screenshot taking utility for the KDE desktop. Spectacle
can also be used in non-KDE X11 desktop environments.

## This fork (branch `mine`)

A personal build of Spectacle 6.3.6 for Debian 13 (trixie) with Plasma 6.3,
adding a few things from [Shottr](https://shottr.cc/). It is not meant for
upstream KDE.

### What's added

**Single-key annotation tools.** While annotating, press a letter to pick a tool:

| Key | Tool | Key | Tool | Key | Tool |
|---|---|---|---|---|---|
| C | Crop | L | Line | X | Pixelate |
| V | Select | A | Arrow | B | Blur |
| P | Freehand | R | Rectangle | T | Text |
| H | Highlighter | E | Ellipse | N | Number |
| M | None (editor only) | | | | |

The keys are off while you type into a text annotation; press Esc to finish
the text. Tooltips show each tool's key.

**Copy at a size.** The toolbar shows `Copy: 25% 50% 100% 200%`:

| Size | Shortcut | For |
|---|---|---|
| 25% | Ctrl+Alt+C | chat apps with upload limits |
| 50% | Ctrl+Shift+C | Discord and the like |
| 100% | Ctrl+C | a normal copy |
| 200% | Ctrl+Alt+Shift+C | pixel art: every pixel becomes a sharp 2x2 block |

**Extract Text (OCR).** The Extract Text button, or Ctrl+O, copies the text in
the screenshot (or in the selected region) to the clipboard, using Tesseract.
A notification offers to open it in a text editor, and Export > Extract Text by
Language reads other languages if their Tesseract data is installed (e.g.
`tesseract-ocr-deu`). It is tuned for screen text: dark and light themes,
coloured bars, sidebars, terminals. Known limit: fonts with a dotted or slashed
zero (e.g. Hack) at very small sizes can read 0 as 8, 6 or @.

The branch also includes the upstream fixes Debian applies to its 6.3.5
package.

### Install

Only for Debian 13 (trixie) with Plasma 6.3. The quick way is the ready-made
package from the [releases page](https://github.com/grymmjack/spectacle/releases):

```sh
wget https://github.com/grymmjack/spectacle/releases/download/v6.3.6-mine2/kde-spectacle_6.3.6-0+mine2_amd64.deb
sudo apt install -y ./kde-spectacle_6.3.6-0+mine2_amd64.deb
sudo apt-mark hold kde-spectacle
```

To build it yourself instead you need `git` and `podman`. The package is built
in a trixie container, so no build dependencies go on your system.

```sh
git clone -b mine https://github.com/grymmjack/spectacle.git
cd spectacle
packaging/build-deb.sh
sudo apt install -y ./packaging/out/kde-spectacle_6.3.6-0+mine2_amd64.deb
sudo apt-mark hold kde-spectacle
```

The first build downloads Debian's image and the build dependencies (about
3 GB, several minutes); later builds are quicker. The package replaces
Debian's `kde-spectacle` and pulls in `tesseract-ocr-eng`. The hold stops apt
from swapping Debian's version back in. Close any open Spectacle window
afterwards so the new one starts.

### Update

Pull, give the build a new version at the top of `debian/changelog` (e.g.
`4:6.3.6-0+mine3`), build, and install it over the held package:

```sh
git pull
packaging/build-deb.sh
sudo apt install -y --allow-change-held-packages ./packaging/out/kde-spectacle_<version>_amd64.deb
```

`build-deb.sh` builds the committed `HEAD`; commit your changes first.

### Go back to Debian's Spectacle

```sh
sudo apt-mark unhold kde-spectacle
sudo apt install --allow-downgrades kde-spectacle=4:6.3.5-2
```

### When Plasma is upgraded

Spectacle has to match the Plasma libraries it runs with. Before upgrading past
Plasma 6.3, rebase `mine` onto the matching upstream `Plasma/x.y` branch and
rebuild; until then the hold keeps this build in place.

---

*The sections below are upstream KDE's and apply to Spectacle itself, not this fork.*

## Contributing

Spectacle is developed under the KDE umbrella and uses KDE infrastructure
for development.

Please see the file CONTRIBUTING for details on coding style and how
to contribute patches. Please note that pull requests on GitHub aren't
supported. The recommended way of contributing patches is via KDE's
instance of GitLab at https://invent.kde.org/graphics/spectacle.

## Release Schedule

Spectacle is released by KDE's release service and has three
major releases every year. They are numbered YY.MM, where YY is the two-
digit year and MM is the two-digit month. Major releases are made in April,
August and December every year. The Spectacle version follows the KDE
release service version.

## Reporting Bugs

Please report bugs at KDE's Bugzilla, available at https://bugs.kde.org/

For discussions, the #kde-devel IRC channel and the kde-devel mailing list
are good places to post.


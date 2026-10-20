# Third-party notices

## JUCE

This project is built on the JUCE framework 9.0.2, which is dual-licensed under
the GNU Affero General Public License v3 and a commercial JUCE licence. This
project is distributed under the AGPL-3.0-or-later; see `LICENSE`.

<https://juce.com> · <https://github.com/juce-framework/JUCE>

JUCE is not vendored in this repository. Clone tag `9.0.2` into `JUCE-9.0.2/`
before building.

## Artwork

`assets/rivage-waterlilies.jpg` is a reproduction of a water-lily painting by
Claude Monet (1840–1926), whose work is in the public domain; the artist died in
1926, so the painting is out of copyright everywhere this project is published.
The file is a crop and resample of a digital copy that was delivered as a design
asset, and that particular scan is not claimed either — what the project relies
on is the public-domain painting. It is used as the plug-in's background, is
embedded into the binary as binary data, and is included for decorative purposes
only. Replace the file, keeping the name, to ship a different image.

## Inno Setup messages

`installer/ChineseSimplified.isl` is the Simplified Chinese message file from the
Inno Setup user-contributed translation collection, maintained by Zhenghan Yang
(Kira). It has been trimmed to the message names that Inno Setup 6.2.2 knows, and
it is used only while packaging the Windows installer — it is not part of the
plug-in binary.

## Fonts

The interface draws with Palatino Linotype, Segoe UI and Consolas. These are
system fonts supplied by the operating system; no font files are distributed
with this project.

## Everything else

All remaining source code, including the six reverb algorithms, the plug-in
wrapper, the headless analysis host and the build scripts, is original work
written for this project.

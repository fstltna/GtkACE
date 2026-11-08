# GtkACE

GtkACE is a graphical client for [Wolfpack Empire](http://www.wolfpackempire.com/),
the multi-player Internet war game, for Linux and other systems with GTK 3.

It is a port of **WinACE 2.5.72** (the Windows Alternate Client for Empire,
written in Visual Basic 6) to C and GTK 3.  GtkACE is a modified version of
WinACE and is distributed under the same Artistic License; it is not WinACE
and is not supported by the WinACE authors.  See "Credits and licence" below.

## Status

Working now (0.3.0):

* Game list with the most recently played games first, and a login dialog
  (host, port, country, password, user, kill hung session, UTF-8, deity,
  AutoRead, AutoUpdate), saved in `~/.config/gtkace/games.conf`
* Connecting without freezing the window, and the full Empire login
  sequence, with clear messages for a wrong password, unknown country or
  unreachable server
* Command line with history (Up/Down); server output in a fixed-width
  window with highlighting; when a command asks a question, the prompt
  shows beside the command line and Escape aborts it
* Telegram and announcement notices in the status bar (both with and
  without `toggle inform`), and AutoRead
* BTU and time used in the status bar
* A local database per game (SQLite, in `~/.local/share/gtkace/`), filled
  with the server's `xdump` and `bmap` output.  Tables are created from the
  server's own descriptions, so newer servers need no client changes.
* The hex map: your sectors, the sea, and what your bmap shows, in WinACE's
  colours.  Click a sector to see its details (efficiency, mobility,
  commodities, thresholds, distribution sector, resources); right-click for
  census, commodity, resource, production, map and designate.  Drag or
  scroll to move, Ctrl+scroll or +/- to zoom, arrow keys move the selection.
* Your ships, land units, planes and nukes on the map with WinACE's unit
  icons (each kind can be hidden from the View menu), listed in the sector
  panel, and in the Units window (Ctrl+1 to Ctrl+4): sortable tables with
  location, efficiency, mobility, tech, cargo and fleet/army/wing.
  Double-click a unit to find it on the map.
* After commands that change sectors or units (move, navigate, build,
  designate, load, fly ...) the map is re-read automatically.
* Intelligence: lookout, land lookout, coastwatch, skywatch, sonar and spy
  output is recorded.  Other countries' sectors appear in their colours,
  their units with WinACE's red (enemy), green (allied), blue (neutral) and
  grey (more than a day old) icons.  The sector panel shows what was seen
  and when; the Units window's Foreign tab lists every sighting.
  Reports -> Intelligence runs the reports; File -> Clear Intelligence
  forgets them.
* Offline mode: open a game's saved map without connecting
* Report menus and toolbar (budget, census, nation, news, power, report,
  starvation, wire, relations, maps and more)

Planned for later milestones: more intelligence sources (radar maps,
recon and satellite pictures, attack and battle reports); the report
windows and the many command dialogs (move, navigate, attack, build,
load ...); the telegram window; colour schemes; and WinACE's tools
(production, build projection, famine relief, fortify, range table ...).

## Building

You need a C compiler, make, pkg-config, and the GTK 3 and SQLite 3
development files.  On Debian or Ubuntu:

    sudo apt install build-essential pkg-config libgtk-3-dev libsqlite3-dev
    make
    ./gtkace

`sudo make install` installs to `/usr/local` (set `PREFIX` to change).

## Using it

Run `gtkace`, choose **New Game**, and fill in your server and country.
**Connect** logs in; with AutoUpdate on, GtkACE then fetches your sectors
and bmap and draws the map.  **Offline** opens the saved map only.

You can also start a saved game directly: `gtkace "My Game"`, or
`gtkace --offline "My Game"`.

Type Empire commands in the command line at the bottom, as you would in
the standard client.  Refresh → Update All (F5) re-reads the map.

GtkACE does not run the `execute` command or redirect output to files or
programs (`>` and `|`); this matches WinACE.

## Credits and licence

WinACE was originally written by Jim Simons (Escher), with collaboration
and testing by Ken McDonald.  It was maintained from v2.2.0 by Daniel
Kiracofe (Bwian) and from v2.3.0 by Ron Koenderink (RimFire), with thanks
to Jon Butler for the original WS Empire code.

WinACE: Copyright 1997-2001 James A. Simons; changes Copyright 2002
Daniel R. Kiracofe and later maintainers.  Released under the Artistic
License (see `LICENSE`).

The unit icons in `icons.h` are WinACE's own pictures (ship.gif and so on),
converted to pixel data.

GtkACE (2026) is a rewrite of WinACE in C for GTK 3.  Every source file is
new; each one names the WinACE modules it was ported from.  As the Artistic
License asks of modified versions, GtkACE has a different name, its
changes are freely available as source, and it is distributed under the
same licence.

# Queen's of Heart '99 SE Netplay

Rollback netplay, modern controller support, training tools, custom character palettes, replays, replay takeover, spectators, window scaling and an in-game overlay for Queen's of Heart '99 SE.

The game itself is not included. Keep a legal, complete copy of a supported build and install this package into it.

- Japanese guide: [README_ja.md](README_ja.md)
- How the netcode, replays and spectators work: [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md)

## Contents

1. [Requirements](#requirements)
2. [Installation](#installation)
3. [Playing online](#playing-online)
4. [Network use](#network-use)
5. [Spectating](#spectating)
6. [Offline modes](#offline-modes)
7. [Training controls](#training-controls)
8. [Custom palettes](#custom-palettes)
9. [Replay controls and takeover](#replay-controls-and-takeover)
10. [Shared display controls](#shared-display-controls)
11. [Caster settings](#caster-settings)
12. [Troubleshooting](#troubleshooting)
13. [Wine and Proton](#wine-and-proton)
14. [Package contents](#package-contents)
15. [Credits and licenses](#credits-and-licenses)

## Requirements

Supported builds:

| Runtime profile | Executables it admits |
|---|---|
| November | The 11/26 retail executable and the 9/4/2006 PovertyHut Jr. community executable |
| December | The December executable |

The authority is an exact hash check of the executable, not its file date. The caster checks the executable and the required game files before it shows the main menu, and explains what to replace if the installation is incomplete, modified or unsupported. Players on different profiles cannot share a session.

You also need:

- Windows with DirectX 9 support, or a recent Wine/Proton setup.
- The complete package. Do not mix its EXE, DLLs or wrapper with files from another release.
- A keyboard or controller. `SDL3.dll` gives the best controller support, so keep it beside the caster.

## Installation

1. Back up your game folder, especially any existing `ddraw.dll`.
2. Extract every file into the game folder, beside the game executable.
3. Run `qoh99caster.exe`. Do not launch the game directly when using this mod.
4. Open Settings -> Player and files... and set your Nickname and Game path. Set Netplay DLL if it was not found automatically.
5. Open Controls and bind Player 1. Bind Player 2 as well for local play.
6. Allow `qoh99caster.exe` through the firewall for UDP when asked.

The eight fighter actions are Up, Down, Left, Right, A (light), B (medium), C (heavy) and D. Controls accepts keyboard keys and controller buttons. A controller's movement can be bound as a whole d-pad or stick with one directional press.

Japanese and Cyrillic nicknames are supported. The caster's own language is the Language row at the top of Settings.

## Playing online

There is no share code and no public lobby. One player hosts, the other types the host's address.

### Host

1. Set the port at Settings -> Connection... -> Host port.
2. Choose Host a game.
3. Give the other player your public IP and port, for example `203.0.113.40:7500`. Press C in the caster to copy the address.
4. After the other player joins, both of you see one screen with the delay and rollback the route needs. Confirm, then pick characters and a stage.

### Join

1. Choose Join a game.
2. Enter the host's numeric IPv4 address as `ip:port`. The port can be omitted when the saved or default port is correct.
3. Confirm the measured delay and rollback, then pick your character.

The caster tries a direct route first and uses automatic hole punching when needed.

### Delay and rollback

There is nothing to configure beforehand. The caster measures the connection and opens the screen on its recommendation, so confirming it is enough. The numbers are never carried over from an earlier match, because they belong to the connection they were measured on.

Both players see each other's choice on that screen as it changes. Your rollback covers what your opponent's input delay does not, so every frame of delay they add is a frame of rollback you do not need, and the same the other way round. The screen shows how much cover each of you has, so you can split a long connection instead of each paying for all of it.

The only values refused are those no setting can rescue: less cover than the time an input physically takes to cross the connection. That is measured from the average ping, not the worst spike, so a low pick on a good connection stays as you chose it. If a value has to be raised when the settings are confirmed, the caster says so. Both sides use the worse of the two measurements.

Input delay goes up to 15 frames and rollback up to 30.

### Online controls

| Default key | Action |
|---|---|
| `-` / `=` | Decrease / increase your input delay |
| `[` / `]` | Decrease / increase your rollback window |
| F1 | End the current match and return both players to character select |
| (no default) | Leave match: bind it under Controls -> Mode Controls if you want a key for it |

The HUD shows the active delay and rollback. Both can be changed at any time during a match, including mid-round: the keys refuse only a value an input physically cannot cross the connection in. Lower delay means less input lag and more correction; higher delay means the opposite. Your rollback protects you from your opponent's connection, and your input delay protects them from yours.

### Character select and rematches

- F1 returns both players to character select and ends the current replay recording.
- At character select, B undoes a character already chosen on that side. With none chosen, B does nothing and cannot go back to the game-mode menu.
- At the continue prompt, if both players choose YES the same matchup restarts immediately. If either chooses NO, both return to character select and stay connected.

### Ports and your router

Players and spectators use different UDP ports. Forwarding only your game port lets people play you but not watch you, so the caster asks your router to open the spectator port while you host and closes it when you stop. It speaks NAT-PMP and UPnP. A router that refuses costs nothing, because hole punching still runs.

| Port | Used by |
|---|---|
| UDP 7500 (your host port) | The game, for the other player |
| UDP 7501 (your host port + 1) | Spectators |

The spectator port is your game port plus one when it is automatic and that port is free; otherwise the system assigns one. Settings -> Let people watch your matches... shows the port, lets you pin it, and lists what to forward by hand.

## Network use

The caster does not only talk to the address you type. This is everything it contacts:

| Destination | When | Why |
|---|---|---|
| The address you enter, and the peers it leads to | Hosting, joining, spectating | Match and spectator traffic |
| `delthas.fr:14763` | When hole punching runs | Public punch service that introduces two machines. It carries no match traffic. Override with `QOH99_PUNCH_RELAY_OVERRIDE`. |
| `https://api.ipify.org/`, then `https://ipv4.icanhazip.com/` | Each caster launch | Detects your public IP so it can be shown and copied. Override with `QOH99_PUBLIC_IP_OVERRIDE`. |
| Your router only: NAT-PMP (port 5351), UPnP (SSDP `239.255.255.250:1900`, then HTTP) | While hosting | Opens the spectator port |

**Firewall.** Allow `qoh99caster.exe` for UDP. Ordinary play has no TCP listener or control channel. Outbound TCP is used only for UPnP, public-IP detection and an optional relay you run yourself.

## Spectating

The host controls access at Settings -> Let people watch your matches...:

| Who may watch | Effect |
|---|---|
| Anyone with my host address | Viewers join without asking |
| Ask me first | Each viewer needs your approval |
| Nobody - do not open a spectator port at all | Spectating is off |

The same page sets the maximum number of simultaneous viewers.

To watch, choose Spectate a game and enter the address a player would use to join, as `ip:port`. No share code. The route is chosen for you: direct, then automatic punching. The public punch service carries no traffic, and no public traffic relay exists. A relay route needs someone to run `qoh99caster relay [--bind IP] [--port N]` and viewers to point `QOH99_RELAY_OVERRIDE` at it.

More than one spectator can watch, within the host's viewer limit, upload speed and computer. Spectators are receive-only and cannot affect player input or match state. Sessions you watch are saved as replays by default; switch off Save spectator sessions under Settings -> Replays and storage....

The replay keys work on your own view only:

| Replay key | While spectating |
|---|---|
| Faster | Skip ahead to live, through any earlier games of the set at full speed |
| Slower | Drop back to normal speed |
| Pause | Pause and resume |
| Step | Advance one frame |

A viewer normally runs about two seconds behind the players, a little more once its connection has stalled. A late joiner catches up on its own: quickly through menus and character select, and during a match at a rate that grows with how far behind it is. While you are 15 seconds or a game behind, the bottom bar periodically names the skip key (3 seconds of every 8).

The spectator window's title bar says what it is doing:

| Title state | Meaning |
|---|---|
| CONNECTING | Reaching the host, no match data yet |
| JOINING | Booting to the match |
| BUFFERING n/m | Filling its cushion; the count shows progress |
| WAITING FOR THE MATCH | Holds everything the players have produced; their game is paused, loading or between rounds |
| CATCHING UP | Behind live, with how far |
| LIVE | Caught up |
| PAUSED | You paused your view |
| SEALED (n left) | The source has finished; playing the remaining n records |
| COMPLETE | Everything has played |
| FAILED | The attempt failed; the logs say why |
| REJECTED | The host refused this viewer |

## Offline modes

Choose Play offline, then:

| Mode | What it does |
|---|---|
| Training | Opens Training mode and its settings panel |
| VS Player | Local player versus player, starting at character select |
| VS CPU | Local player versus CPU, starting at character select |
| Free Play | Opens the game's native menu for its regular offline modes |

VS Player and VS CPU count completed games for the whole session and show each side's total as "Wins: N" where online play shows nicknames.

### The opening movie

Free Play starts with the game's opening movie. Where the game's opening movie uses Indeo 5, Windows 10 and 11 contain the codec but do not register it, which gives a black screen with only music. The packaged `ddraw.dll` registers the decoder for the game alone while it runs. Nothing is installed or changed on the system, and the movie file is untouched. The bundled `ir50_32.dll` is tried first and the Windows copy is the fallback, so the DLL is optional.

This works whether the game is started through the caster or directly, and the picture scales to the window. Any attack button skips it. Online play, spectating, replays and Training never show it.

### Native offline F-keys

These apply only to VS Player, VS CPU and Free Play, and only after you enable them. Training uses its own controls and ignores them.

| Key | Action |
|---|---|
| F2 / F3 | Toggle CPU control for Player 1 / Player 2 |
| F4 | Toggle HP/SP automatic refill |
| F5 | Return to character select |
| F6 | Reset the round |
| F7 | Show or hide native hitboxes |
| F9 | Pause; F8 advances one frame |
| F12 | Freeze |
| Delete | Quit the match |

To enable them, close the game, then in the caster go to Settings -> Extra functions (hidden F-keys, FCommand.txt)..., set Hidden F-keys (F2..F9) to TRUE, and choose Save to FCommand.txt. That alone enables F8 and F9 as well. The file is `LocalConfig\FCommand.txt`.

## Training controls

Player 1's configured input selects both characters in sequence. Confirm the first character, release the controls, confirm the second, then pick the stage.

| Default key | Action |
|---|---|
| 1 | Reset positions |
| 2 | Save state to the selected slot |
| 3 | Load the selected slot |
| 4 | Place the dummy, then record it (see below) |
| 5 | Start / stop dummy playback |
| 6 / 7 | Previous / next slot |
| 8 or F8 | Pause / resume |
| 9 or F9 | Advance one frame and pause |
| F1 | Return to character select |
| F2 | Cycle dummy stance |
| F3 | Cycle dummy guard mode |
| F4 | Toggle "Restore all": keeps HP, EX and guard gauge full while both characters are idle, walking or crouching |
| F5 (hold) | Charge the fractional EX meter continuously |
| F6 | Reset to round start and force both characters idle |
| F7 | Show / hide hitboxes, hurtboxes and collision boxes |
| Insert, or controller Start | Open / close the training panel |
| Backspace | Show / hide the input viewer |

These are defaults. Rebind them under Controls -> Mode Controls, from keyboard or the selected controller. F10, F11, F12 and Delete cannot be bound to Training actions.

### Recording the dummy, in three presses

Key 4 is one key with three steps. The line above the health bars says which press comes next.

| Press | What happens |
|---|---|
| First | Placing. Your controls drive the dummy and your own fighter stands still. Nothing is kept, however long you take, so walk it to the spacing you want to practise against. |
| Second | Recording starts, from where you left the dummy. |
| Third | Stops, keeping what was recorded. |

While placing, key 5 (or the Stop row in the menu) leaves without recording and without playing the slot. The recording already in that slot is untouched either way.

### Reset position modifiers

| Input | Position |
|---|---|
| 1 alone | Round start |
| 1 + Left | Left corner |
| 1 + Right | Right corner |
| 1 + Down | Center |

Reset position moves the fighters without resetting health, meter, timer or round count. Save states and recordings have eight slots and are discarded when the training session ends.

### EX charge

F5 adds a stock whenever the fractional meter (the small bar beside the stock count) fills, then begins filling again. At nine stocks, pressing or holding F5 clears the stocks and starts the same cycle from zero.

### The training menu

Insert opens the Quick, Dummy, Triggers, Recording, View, Palette, Keys, Help and Diagnostics tabs. Long pages split into a second row of small tabs; switch with Left and Right, or click. Restore full HP restores life only. Restore all restores life, nine EX stocks and the full guard gauge. Automatic restore options act once both characters have been idle long enough and a selected resource is not full; they do not wait for another combo.

| Option | Where | What it does |
|---|---|---|
| Player swap | Menu toggle "Dummy is P1 (you play P2)", no default key | Chooses which fighter is the dummy: the one that blocks and plays back recordings |
| Guard timing | Dummy tab, Guard | When the dummy blocks. "Always" is the usual behaviour. "After being hit" lets the first hit land, blocks for the configured window, then drops guard once the pressure ends; good for frame traps. "Until first block" blocks one hit and stops, which shows whether the rest of your string would connect. The window row sets both timers in frames. |
| Damage reduce | Dummy tab, Defence | Pressing attack buttons quickly while being comboed cuts each hit to 90 percent, or 70 percent when mashing fast. The game does not document this. Per side: Manual for the real check, force either strength, or Off for clean damage numbers. |
| Critical hits | Quick tab, Values | Per attacking side: normal chance, always, no random criticals, fully disabled, or force the roll |
| Overlay switches | View tab, Overlay | Each box type per player, including the centre axis line the game's own display draws. Frame advantage and the input viewer can be limited to one side. |
| Random playback | Recording tab, Playback; Triggers tab | "Random" plays one recorded slot at random, whichever is selected. "Random loop" picks again after every run. On Triggers, "Random recording" answers an event with a random recorded slot each time it fires. |

Training also reports on screen, above the frame advantage line. Saving, loading, clearing, resetting and changing slot appear briefly and fade. Placing, recording and playing stay until they end and name their slot.

## Custom palettes

A palette is a small text file named `CHARACTER_N.pal`, for example `SAORI_2.pal`, in the `palettes` folder next to the game. N is the colour slot 1 to 4 you pick at character select.

### Wearing one

While picking your colour at character select, press D. The match starts with the file for that character and slot; if it does not exist, the normal colours are used. This works offline and online.

Your opponent sees your palette online, spectators see it, and replays store it. Online, only your own character can wear your files. When a player picks a custom palette, the rule that both players cannot take the same colour slot is lifted.

During a match, Tab switches between the custom palette and the game's colours. Online, Tab only changes how the opponent is shown; your own palette stays on. To use another key, set `PaletteToggleKey` under `[Overlay]` in `ddraw_wrapper.ini`.

### The palette editor

Training has a Palette tab in the in-game menu (Insert). It edits colours live on the character:

- Pick the slot (P1 or P2) and, offline, the palette 1 to 4.
- Click a colour in the grid for the full picker, with RGB, HSV and hex entry plus brightness and saturation sliders.
- Double-click a number row to type an exact value.
- The Undo / redo row steps through your changes. Ctrl+Z and Ctrl+Y work too, including inside the picker.
- Save palette writes to the file named on the row. Load palette reads it back. Load default palette restores the game's colours. Restore match colours returns to what this match started with.
- A live preview shows your character and anything it has spawned, such as a projectile.
- The in-game D button hides the menu so you can watch the full screen; press it again to come back.

Saved files are normal JASC palette text, so any palette-capable image tool can edit them.

## Replay controls and takeover

Open Replays from the main menu to browse and play `.q99r` files. Recording is set under Settings -> Replays and storage....

| Session | Recorded? |
|---|---|
| Netplay | Yes, default on |
| Local VS and Free Play | Yes, default on |
| Spectator sessions | Yes, default on |
| Training, replay of a replay, VS CPU | Never |

### In the replay list

| Key | Action |
|---|---|
| Enter | Watch the selected replay |
| P | Play every replay in the selected folder (or the open folder when a replay is selected) |
| D | Details for the selected replay |
| Tab | Change the sort order |
| F | Favourite / unfavourite |
| Del / U | Delete / undo the delete |
| R | Rescan the folder |
| C | Clear the search or filters |
| Left / Right | Page the list |
| L | Fill in the battle log from replay files, when some games are missing from it |
| Esc | Back |

The default sort is Game order: newest sets first, with Game 1, 2, 3... within each set. Replays without set and game data appear by recording date, newest first. Alphabetical (A-Z) sorts titles and names. Match count (most first) sorts by the number of matches stored in a recording, not its game number in a set.

The list scans when opened and when you press R, and keeps the current list and selection visible until the scan finishes.

Folder playback starts with the first recorded game and keeps one game process and window throughout. The first game follows the Pause at replay start setting; following games start automatically with your display settings. The last win screen stays for one second, then playback ends. Exiting a replay or a failed playback stops the sequence. Search and sort do not limit it. It covers files directly in that folder, not subfolders.

The battle log sits beside the list. On a replay it shows that game's result, its set and place in that set, and your record against that opponent. Pick a set folder and it shows that set. Games join the log by their game id, so a renamed or moved replay still lands in the right set.

### Playback

| Default key | Action |
|---|---|
| Space | Pause / resume |
| Right Arrow or `.` | Advance one paused frame |
| Left Arrow or `,` | Go back one frame and pause |
| Home | Return to the beginning and pause |
| `-` or Numpad `-` | Play slower |
| `=` or Numpad `+` | Play faster |
| F7 | Show / hide hitboxes, hurtboxes and collision boxes |
| Backspace | Show / hide both players' input history |
| F | Take over Player 1 |
| G | Take over Player 2 |
| Q | Exit takeover and return to the replay |
| R | Retry the takeover branch |
| 9 / 0 | Decrease / increase takeover input delay |

Rebind these under Controls -> Mode Controls.

Replays play automatically. To pause at the first frame, turn on Settings -> Replays and storage... -> Pause at replay start, saved as `replayPauseAtStart=1` in `qoh99caster.ini`. The frame counter and speed are visible, with a paused hint naming your resume key. Input history and hitboxes start hidden.

Stepping back restores a nearby in-memory checkpoint and replays the short gap to the chosen frame. Checkpoints start 16 frames apart; very long recordings space them wider to keep the cache within 256 MiB. Checkpoints are discarded when playback closes.

### Takeover

Press F or G at the frame you want. The replay pauses for a countdown, then your Player 1 control profile drives the selected fighter while the other continues from the recording. The branch is temporary: Q restores the replay state, and R retries from the saved branch point. The delay is shown on screen and saved in `ddraw_wrapper.ini`.

Replays made with an incompatible game or mod version cannot be played.

## Shared display controls

| Default key | Action |
|---|---|
| F11 | Toggle windowed and the preferred fullscreen mode |
| Insert | Open / close the in-game panel where the mode allows it |
| Backspace | Toggle the input viewer in Training and Replay |

A controller's Start button also opens the in-game panel. All of these are configurable under Controls -> Mode Controls.

The window is resizable and keeps the game's aspect ratio. Display settings live in `ddraw_wrapper.ini`, with a short comment beside each option. The caster menus and in-game controls normally cover them.

## Caster settings

Use `qoh99caster.exe` for normal configuration.

| Menu | What is there |
|---|---|
| Controls | Fighter bindings and all wrapper, online, training and replay controls |
| Settings -> Language | Switches the caster between English and Japanese |
| Settings -> Player and files... | Nickname, game executable and injected DLL path |
| Settings -> Connection... | Host port and the built-in hole-punch service information |
| Settings -> Audio... | Music and sound-effect volume |
| Settings -> In-game display and keys... | Native HUD visibility and the in-game panel reminder |
| Settings -> Game config (fullscreen, music, vsync)... | The game's display, music and timing options |
| Settings -> Extra functions (hidden F-keys, FCommand.txt)... | The game's native offline developer options |
| Settings -> Let people watch your matches... | Who may watch, viewer limit, spectator port, what to forward |
| Settings -> Replays and storage... | Recording, replay folder, browser filters, storage warnings |

Files written beside the caster or game:

| File | Holds |
|---|---|
| `qoh99caster.ini` | Main caster settings |
| `qoh99caster-controls.json` | Fighter and mode controls |
| `ddraw_wrapper.ini` | Window, renderer, HUD and replay display |
| `LocalConfig\FCommand.txt` | Native offline developer settings |
| `System\QOHcnf.key` | The game's own configuration |
| `palettes\` | Custom palette files |
| `replays\` | Recorded matches, unless changed |
| `logs\session_*` or `logs\session_*.zip` | Diagnostics for failed runs, zipped once archived |

Close the game before changing `QOHcnf.key` or `FCommand.txt`. A backup of the previous working file is kept when either is saved.

## Troubleshooting

### Antivirus false positives

If your antivirus calls this a virus, or a file keeps disappearing, it is a false positive. Microsoft Defender is the usual one.

This mod starts the game paused, loads `qoh99_netplay.dll` into it, then lets it run. That is the only way to add rollback netplay, training and replays to a 1999 game without source code. Real malware uses the same technique, so automatic scanners flag its shape without looking at what the code does. The step cannot be removed without removing netplay.

It adds no startup entries or registry persistence, and does no hiding, packing or obfuscation. Everything it contacts is listed under [Network use](#network-use).

If a file has already been removed:

1. Open Windows Security -> Virus & threat protection -> Protection history.
2. Find the detection and choose Restore. Restore first: an exclusion does not bring back a file already taken.
3. Run `defender_exclusions.cmd` as administrator, or add the folder by hand under Virus & threat protection -> Manage settings -> Exclusions -> Add an exclusion -> Folder.

`defender_exclusions.cmd` is plain text, so read it first. It adds this one folder plus process exclusions for `qoh99caster.exe`, `qoh99_netplay.dll` and `ddraw.dll`. It does not switch off your antivirus. Undo it with `defender_exclusions.cmd remove`. The trade-off: an excluded folder is not scanned, so only exclude a folder whose contents you trust.

If the caster warns at startup that a file is missing, this is the same problem. Submitting the files at https://www.microsoft.com/en-us/wdsi/filesubmission (choose "Software developer" -> "Incorrectly detected as malware") gets the detection removed for everyone, usually within a few days.

### The game stays open after I close the caster

It should not. The game is tied to the caster and closes with it, even when the caster is killed. If the game survives, that is a bug worth reporting.

### My F-keys do not work in VS Player, VS CPU or Free Play

Close the game, then set Settings -> Extra functions (hidden F-keys, FCommand.txt)... -> Hidden F-keys (F2..F9) to TRUE and choose Save to FCommand.txt. If saving fails, make sure the game is closed and `LocalConfig` is writable. Training and Replay do not use `FCommand.txt`; rebind their keys under Controls -> Mode Controls.

### The game does not display effects correctly

Close the game, then set Settings -> Game config (fullscreen, music, vsync)... -> 16-bit color to yes and choose Save to QOHcnf.key.

### F11 does not resize the game, or it opens in a tiny fixed window

In Settings -> Game config (fullscreen, music, vsync)..., set Fullscreen to "yes (normal - resizable window + F11)", save and relaunch. The game's internal fullscreen path is what lets the wrapper provide resizable windowed, borderless and exclusive modes.

### The window is too large after returning from fullscreen

Close the game. In `ddraw_wrapper.ini`, delete `ClientWidth` and `ClientHeight` together, or set a smaller `WindowScale`. A safe default size is rebuilt on the next launch.

### The game has no music

In Settings -> Game config (fullscreen, music, vsync)..., set Music to Wave and make sure the music folder points to the game's `System\Bgm`. CD audio is silent without the original disc. Also check that Music volume under Settings -> Audio... is not zero.

### The caster says the game build is wrong or missing

Use an exact supported executable (see [Requirements](#requirements)) with its proper game content, and set the path under Settings -> Player and files.... Do not rename an unrelated build or copy one edition's executable over another installation.

The file date of `GAME.EXE` or `qoh99.exe` is a rough guide only: 11/26 for November, 12/12 for December, 9/4/2006 for PovertyHut Jr. The hash check decides.

### The caster says my opponent has an older or newer mod build

Both players must install the same complete release. Replace the caster, `qoh99_netplay.dll`, `ddraw.dll`, `SDL3.dll` and the supporting files together, then restart both casters.

### Online play does not connect

- Recheck the numeric `ip:port` and the host port under Settings -> Connection....
- Allow `qoh99caster.exe` through the firewall for UDP.
- Do not run two hosts on the same port and address.
- If direct play and automatic punching both fail, collect all peers' logs.
- As a last resort, use Radmin VPN, Hamachi or a similar VPN, and connect with its assigned IP and the port the caster shows.

### Spectating times out, or the picture does not start

- Read the spectator window's title bar first. WAITING FOR THE MATCH means the connection is fine and the players' game is not advancing. BUFFERING with a count that is not rising means data is not arriving. REJECTED means the host refused.
- Confirm the host selected "Anyone with my host address" or "Ask me first", not "Nobody".
- Confirm the viewer limit is not full.
- Enter only the host's numeric `ip:port`, with no prefix or share code.
- Collect the spectator, host and client session logs from the same attempt. The route log records where and why it failed.

### A replay will not play

Use the complete bundle and supported game files. The replay browser and log show why a replay was rejected. Do not edit the replay file.

### The game runs too fast, too slow or stutters offline

Return custom `FCommand.txt` timing fields to their defaults unless you understand them: Frame limiter (QPC) unset, WaitCount off, ExtraSkip off, SetWaitTime auto-measured. The game normally runs internally at about 60.60 frames per second.

### A controller is missing or has odd button names

Keep the packaged `SDL3.dll` beside `qoh99caster.exe` and reopen Controls. Select the correct controller separately for each player. Unplug duplicate or unused devices if Windows reports them ambiguously.

### Crashes, unusual game behaviour or netplay lag

Normal sessions write no diagnostic logs. Recent context stays in a bounded memory buffer; crashes, desyncs and reported runtime failures save it along with later diagnostics. For a full support capture, set `QOH99_LOG=1` before launching the caster. `QOH99_WRAPPER_LOG=2` adds verbose renderer diagnostics. A hard kill or power loss may leave no text log.

Do not mix files from different releases. Keep the whole `logs\session_*` folder or `session_*.zip` from the failed run, including `dll.log` and any crash dump, and collect the same attempt's log from the other peer when online.

### The game closes by itself, for example right after the intro

The game does this when a file it needs is missing or damaged. Its reason is in `SYSTEM\QOHerror.txt` and in the logs as lines starting with `[game-error]`, in `dll.log` and at the end of `client.log`. A line such as `error:.\System\title\wall.img` names the exact file. Copy the complete game folder again from a source known to be good.

## Wine and Proton

Support is not verified on real Linux hardware. Treat it as best effort.

The package includes `launch_wine.sh`, which launches the caster with the required Wine overrides. From the game folder:

```
bash launch_wine.sh
```

| Flag | Effect |
|---|---|
| `--check` | Run the checks and exit without launching. Paste this output when asking for help. |
| `--recheck` | Ignore the stored marker and check again before launching |
| `--game-dir <path>` | The folder holding `qoh99.exe`. Defaults to the script's folder. |
| `--strict` | Treat any check failure as fatal and refuse to launch |

The script recognises only the November executable's profile. A December executable is reported as failing its check and still launches, unless you pass `--strict`. The caster stays the authority on what it admits.

You can also set the `ddraw.dll` override and launch the caster through Wine by hand.

## Package contents

| File | Purpose |
|---|---|
| `qoh99caster.exe` | Main application, game launcher and relay service |
| `qoh99_netplay.dll` | Netplay, training and replay features |
| `ddraw.dll` | DirectDraw wrapper, renderer, HUD and overlay |
| `SDL3.dll` | Controller runtime |
| `launch_wine.sh` | Wine/Proton launcher |
| `ir50_32.dll` | Optional Intel Indeo 5 decoder for the opening movie |
| `defender_exclusions.cmd` | Optional Microsoft Defender exclusions. Plain text; read it before running it. |
| `README.txt` | This guide and all license notices |
| `README_ja.txt` | This guide in Japanese. The license notices stay here in their original wording. |
| `manifest.sha256` | SHA-256 manifest for the complete package |

The game executable, data, music, voices and other copyrighted game assets are not distributed by this project.

## Credits and licenses

Thanks to Ev.Genii, Wachen, the Lair of the Button Mash community, and the off-topic channel of the EFZ server for testing. Special thanks to yuhi for thorough testing and feature suggestions.

CCCaster, EfzRevival and Giuroll were used as references.

This project is released under the [MIT License](LICENSE), printed in full below. It covers this project's own code only. The third-party notices below apply to their named components, and nothing here grants rights to the commercial game or its assets.

Two bundled files carry third-party copyright and are not covered by this project's license:

| File | What it is | Copyright |
|---|---|---|
| `assets/ir50_32.dll` | Intel Indeo 5.10 video decoder | Intel Corporation |
| `assets/sounds/opponent_joined.wav` | Windows system sound "Windows Background.wav" | Microsoft Corporation |

### License

```
MIT License

Copyright (c) 2026 Aquat1c

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### Third-party notices

```
-------------------------------------------------------------------------------
MINIZ LICENSE
-------------------------------------------------------------------------------

miniz 3.1.2 - https://github.com/richgel999/miniz
Used by qoh99caster.exe to compress finished session log folders in-process.

Copyright 2013-2014 RAD Game Tools and Valve Software
Copyright 2010-2014 Rich Geldreich and Tenacious Software LLC

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


-------------------------------------------------------------------------------
MINHOOK LICENSE
-------------------------------------------------------------------------------

/*
 *  MinHook - The Minimalistic API Hooking Library for x64/x86
 *  Copyright (c) 2009 Tsuda Kageyu.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 *  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 *  OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
/*
 * Hacker Disassembler Engine 32 C
 * Copyright (c) 2008-2009, Vyacheslav Patkov.
 * All rights reserved.
 *
 */

/*
 * Hacker Disassembler Engine 64 C
 * Copyright (c) 2008-2009, Vyacheslav Patkov.
 * All rights reserved.
 *
 */


-------------------------------------------------------------------------------
DEAR IMGUI LICENSE
-------------------------------------------------------------------------------

The MIT License (MIT)

Copyright (c) 2014-2025 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.


-------------------------------------------------------------------------------
SDL3 LICENSE
-------------------------------------------------------------------------------

Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

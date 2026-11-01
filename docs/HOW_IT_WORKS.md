# How Queen's of Heart '99 SE Netplay works

The player manual is [README.md](../README.md). This document covers what happens underneath it: first how the features behave as systems, then how each subsystem is built, and finally where to make common changes and how to build and test.

## Part 1: Using the mod

This part follows what a player does and shows the code that runs. Key tables, menu layouts and troubleshooting live in the [README](../README.md); nothing here repeats them.

Three programs cooperate:

| Program | Owns | Lives in |
|---|---|---|
| `qoh99caster.exe` | Menus, sockets for control traffic, settings files, launching and injecting, replay library, recorder and broker | `src/client/Main.cpp`, `src/client/replay/`, `src/client/spectator/` |
| `qoh99_netplay.dll` | Everything on the game thread: hooks, rollback, training, replay runtime | `src/GameHooks.cpp`, `src/training/`, `src/game/replay/` |
| `ddraw.dll` | Presentation, the in-game menu, HUD, overlays, palette tables | `ddraw_wrapper/render_dx9_imgui.cpp` |

The caster hands the DLL its whole configuration once, as a `qipc::IpcConfig` (`src/ipc/IpcConfig.h`), at launch. The DLL and the wrapper never read `qoh99caster.ini` or `qoh99caster-controls.json`. Replay data crosses named pipes (`ipcRecorderPipeName`, `ipcReplayBrokerPipeName`). The DLL and wrapper talk through exports listed in `ddraw_wrapper/ddraw.def`, resolved with `GetProcAddress`, so either side tolerates the other being absent.

### How an online session runs

`interactive()` in `src/client/Main.cpp` is the main menu. It checks the game files first (`gameFilesCheck`) and starts public-IP detection (`qclient::public_ip::start`). Host and Join call `runAutomaticHost` and `runAutomaticJoin`; Spectate calls `queryHostSpectateInfo`, then `runSpectator`.

**Two sockets on the host.** Control traffic is a reliable stream carried over UDP (`qnet::UdpControlLink`, `src/net/ControlLink.h`), served by `HostControlEndpoint` on the configured port. That is the port players share. Match traffic uses a separate OS-assigned UDP socket opened with `udp.open(0)`, and the host tells the joiner about it during negotiation (`hostInit.dataPort`, plus route candidates). A third socket, from `prepareSpectatorEndpoint`, serves spectators. There is no TCP listener.

**Step 1: reach the host's control port.** `dialHostControl` wraps `qnet::dialControlLink`, which does not run direct and punching in sequence. It sends `LinkHello` to the typed address every `kLinkHelloIntervalMs` while an `AutopunchSession` asks the public service (`delthas.fr:14763`, `qnet::autopunch::kRelayHost`) for the host's NAT mapping. When a mapping arrives it joins the hello targets through `addHelloTarget`, and whichever address answers first wins. The budget is 15 s when a relay fallback exists and 20 s when it does not (`kDialWithFallbackMs`, `kDialOnlyRouteMs`). Hosts register with the same service from `HostControlEndpoint`, and `AutopunchPeerSet` punches back at each visitor it learns about.

**Step 2: compatibility and intent.** The first messages are `MSG_VERSION_CONFIG` and `MSG_CLIENT_INTENT`. Intent is one of `INTENT_JOIN`, `INTENT_SPECTATE`, `INTENT_STATUS`, `INTENT_SPECTATE_RELEASE` or `INTENT_CLIENT_ROOT`, so one port serves everyone. A viewer knocking before a match starts gets an answer and `hostNegotiate` returns `HostAttempt::LoopBack`, leaving the host waiting. Mismatched versions and mismatched game content are refused here (`localCompatibilityIdentityFor`), never mid-match.

**Step 3: choose the match route.** This happens in `runJoin`, on the match socket:

1. `raceDirectCandidates` probes the typed address plus the host's advertised candidates in parallel for up to 1500 ms. A candidate qualifies at 8 unique replies; nothing is scored before 300 ms.
2. `qnet::selectRoute` (`src/net/RouteSelection.h`) picks the lowest p95 latency. Within `tieEpsilonMicros` (3 ms) a LAN route beats a public one, then lower loss wins.
3. If nothing validates, `autopunchFallbackPass` spends up to 4000 ms getting a mapping from the punch service, and the mapping is raced alone.
4. `clientUdpRendezvous` commits to the route.

**Step 4: the optional relay.** A relay carries traffic, so it exists only if someone runs `qoh99caster relay` and points clients at it with `QOH99_RELAY_OVERRIDE`. `configuredRelayServer` reads that variable and nothing else; the settings file has no relay field and no public relay exists. Without it, `automaticRelayOptions` fails and `runAutomaticJoin` goes direct-only. With it, a failure at the dial, version or rendezvous stage makes `runJoin` call itself again with the relay options. Both sides derive the room name from the host's address (`deriveEndpointRoomId`), so nothing extra is shared. In that mode `qnet::relay::RouteSelector` (`src/net/RelayRoute.h`) probes the direct route immediately and the relay only after `kDirectGraceMs`. A direct route wins if it establishes no later than the relay, the first route selected stays selected, and the attempt ends at `kRouteDeadlineMs`.

**Step 5: measure, then choose delay and rollback.** Once a route is committed, `sampleCommittedRoute` collects probes for up to 2 s, and `computeRouteRecommendation` derives a floor from them. The joiner sends it as `MSG_TUNING_PROPOSAL`, both casters show the "Connection measured" screen, and the two floors combine worst-case. Nothing is carried over between sessions, by design: `settingsMenu` has no delay or rollback page. The hard limits are `kCapabilityDelay` (15) and `kCapabilityRollback` (30) in `src/net/PacingBudget.h`.

**Step 6: launch.** Each caster launches its game with the negotiated `IpcConfig` and stays alive as the session owner; the game process cannot outlive it. From here `serveIntentDuringMatch` answers visitors on the control port without touching the players.

Invariant: route selection only ever runs before a match. A failed relay lookup, a spectator knock or a dead punch service may delay setup but cannot affect a running match. `prepareSpectatorEndpoint` and the port mapper log and continue on failure for the same reason.

### Spectating

Watching has two phases. **Admission** runs on the host's control port. **Data** runs on the dedicated spectator port.

The spectator port is `spectatorPort` if set, otherwise game port + 1. If that is busy, `prepareSpectatorEndpoint` walks up to 8 ports higher, and only then accepts an OS-assigned one. `startSpectatorPortMapping` asks the router to open it (`src/net/PortMapper.h`), best effort. The player port is never mapped.

**Admission policy.** `spectatorAdmission` in the ini stores the raw value of `AdmissionPolicy` (`src/client/spectator/SpectatorSidecar.h`). `admissionPolicyOf` reads it and treats anything unknown as `Public`.

| Menu text | Stored | Enum | Effect |
|---|---|---|---|
| Anyone with my host address | 1 | `Public` | Grant issued automatically |
| Ask me first | 2 | `AskHost` | The host's console shows a "Spectator request" menu per viewer |
| Nobody - do not open a spectator port at all | 4 | `Disabled` | No socket is opened; `prepareSpectatorEndpoint` returns early |

`CodeOnly` (3) exists in the enum and the admission code, but no menu row sets it, and `load` rewrites a stored 3 to 2. Only the internal client-private service runs under it.

**The exchange.** It takes two requests:

1. `queryHostSpectateInfo` sends `INTENT_SPECTATE` with session id 0. `serveIntentDuringMatch` answers with `SpectateInfo`, and session id 0 under `Public` or `AskHost` deliberately issues nothing (`SPECTATE_ADMISSION_NONE`). The viewer learns the session id and key without using a slot. Before a match starts, `hostNegotiate` answers instead with `SPECTATE_NO_MATCH`, and the menu offers "Join as the opponent" or "Wait for their match to start". `Disabled` clears the secrets, and the menu reports the refusal.
2. `runSpectator` calls `preflightSpectateWith` (30 s budget), which sends the intent again through `controlExchange`, now with the session id and key. `sidecarIssueGrant` either signs a `SpectatorGrant` with a 300 s lifetime or refuses it.

**Viewer limit.** `spectatorMaxViewers` (1 to 256, default 32) becomes `maxViewers` in the sidecar config. The sidecar also enforces `maxViewersPerIp` (4), and `CapacityFull` and `PerIpQuota` both surface as `SPECTATE_ADMISSION_CAPACITY`. Under `AskHost`, the grant is issued first and held; `sidecarApproveViewer` or `sidecarDeclineViewer` then settles it. A viewer the caster admitted but never used is released by `SpectatorAdmissionReleaseGuard`, so a failed launch does not hold a slot.

**Route.** After admission, the preflight probes direct for `directRaceMs` (3000 ms), then runs a punch assist for `kAutopunchAssistMs` (10 s), then tries a traffic relay if one is configured. The same rule as for players applies: the public punch service carries no match traffic.

**Isolation.** The viewer process plays a stream of confirmed inputs in its own copy of the game. It sends nothing a player's simulation reads, which is why a viewer cannot affect a match. The window title states (`JOINING`, `CONNECTING`, `BUFFERING`, `WAITING FOR THE MATCH`, `CATCHING UP`, `LIVE`, `PAUSED`, `SEALED`, `COMPLETE`, `FAILED`, `REJECTED`) are the first diagnostic to read. See the README for what each means to a viewer.

### Training

Training is an offline launch: `offlineMenu` calls `runOffline` with `qipc::MODE_OBSERVE`, never `MODE_NETPLAY`. `src/training/TrainingGameIntegration.inl` is compiled into `GameHooks.cpp` and owns all the mutable state. The classes beside it (`TrainingMacros`, `TrainingInput`, the resource triggers) are pure policy with tests and no game access.

The per-frame order is the thing to learn:

```
hkTrainingStepGate -> trainingAdmitAfterNativeGate
  trainingDrainCommands            queued menu commands
  trainingApplyActionEdges         key edges
  trainingCaptureSnapshot          stable scratch image for this step
  trainingAdmitPausedFrame         a paused frame stops here
  ++g_trainingStepSerial
  trainingComposeInputForStep
    TrainingMacros::advance        what does the macro want this step?
    g_trainingRecovery.advance     recovery tech emission
    guard and posture decision     trainingGuardDecision, chooseDummyPosture
    TrainingInput::advance         merge everything into two lane masks
    commitRecordedFrame            store what the dummy really received
  native simulation step reads g_trainingFinalInput
```

Native F-keys stay suppressed in Training, because each one mutates state the step invariants assert on.

#### How keymaps reach the game

Every bindable control is a row in `kModeControlDescriptors` (`src/input/ModeControlContract.h`): action, domain (`Training`, `Replay`, `OnlinePlayer`, `WrapperGlobal`), trigger, device policy and stable JSON name. Defaults are in `installDefaultModeControls`, as keyboard scan codes rather than virtual keys.

1. The Controls page writes `qoh99caster-controls.json` atomically (`kControlsFileName`, with a `.bak`), and replaces the `[Hotkeys]` section of `ddraw_wrapper.ini` from the same bindings, which is where the wrapper reads its own keys. It detects clashing bindings through `findModeControlConflicts`.
2. At launch `compileInputProfile` compiles the profile into `CompiledModeControlsV1`, which travels in `IpcConfig`.
3. Each poll, `ModeControlEvaluator::evaluate` turns device snapshots into `edges` and `held` bitmasks for the enabled domain. A control held through a focus change or a device change does not fire, because the evaluator must see it released first (`armed_`).
4. `GameHooks.cpp` maps those bits onto `TrainingAction` bits, one `mapHeld` call each.
5. `TrainingInput::pollActions` applies the training gate. While the menu is captured it disarms everything, then walks `InputRearmState` from `AwaitNeutral` through `NeutralSettle` to `Armed`. An operation like a state load only disarms the action keys until they are released, and the next step is forced neutral. A direction used as a reset modifier goes into `carryMask_`, which keeps it off the lane until it is released.

Fighter direction bits double as reset modifiers (`positionPresetFromResetModifier`), which is why Reset Position takes "1 + Left". Two actions, `RefillAll` and `ChargeExFraction`, are level-triggered (`kTrainingLevelActionMask`); the rest are edges.

The menu takes a different route to the same state. It runs on the render thread and calls `EnqueueTrainingCommand`, which feeds `TrainingCommandQueue`, a lock-free single-producer queue that the game thread drains in `trainingDrainCommands`, applying each command with `trainingApplyCommand`. Commands carry `trainingGeneration`, so a command from a previous session is rejected rather than applied to the wrong match. Status comes back through `TrainingStatusPublisher`. Keys and menu never share a lock.

#### Dummy recording and playback

`TrainingMacros` is a state machine over `MacroState`:

```
Idle --(key 4)--> PreRecord --(key 4)--> RecordArmed -> Recording --(key 4)--> RecordStopping -> Idle
                      \--(key 5 or Stop)-----------------------------------------^
Idle --(key 5)--> PlayArmed -> Playing --(end or key 5)--> PlayStopping -> Idle
```

The armed and stopping states exist so every transition takes effect on a real step boundary. `advance` takes a monotonically increasing step serial: a repeat serial returns the cached result and a lower one fails, so re-entering a frame cannot record or play twice.

| State | Your fighter's lane | Dummy's lane |
|---|---|---|
| `Idle` | Your input | Guard and posture policy |
| `PreRecord` | Forced neutral | Your input, nothing stored |
| `RecordArmed` | Forced neutral | Neutral; clears the slot on this step |
| `Recording` | Forced neutral | Your input, stored |
| `Playing` | Your input | Slot data, with optional guard |

The slot is cleared only on the `RecordArmed` step, so cancelling from `PreRecord` leaves the old recording untouched. That is the promise the README makes.

What gets stored is the dummy lane's mask **after** `TrainingInput::advance` has merged and cleaned it, passed to `commitRecordedFrame` together with the facing on that step. Recording happens in two parts (`recordFrameRequested`, then commit), and a commit whose serial does not match fails closed to `RecordStopping`. A `MacroFrame` is one `logicalMask` plus `recordedFacing`, with capacity `kTrainingMacroFrameCapacity` (3600 frames, one minute) per slot.

At playback, `FacingPolicy::Relative` calls `remapFacingRelative`, swapping Left and Right if the dummy now faces the other way, so a recorded forward stays forward. `Absolute` replays the mask as stored. `PlaybackMode` adds `Loop`, `Random` and `RandomLoop`; the random modes choose among slots with `frameCount != 0` through `pickRandomRecordedSlot`.

**Dummy settings follow the fighter that is standing there.** While you drive the dummy lane, the dummy settings (guard, recovery) act on your own idle fighter instead, `policyFollowsHuman` in `trainingComposeInputForStep`. Otherwise your recording would run against an opponent with no defense. Guard timing state restarts when that flips. Player swap (`g_trainingPlayerSwap`, `TrainingCommandKind::SetPlayerSwap`, the "Dummy is P1 (you play P2)" row) mirrors the same logic and has no default key. Input masks swap around the macros, so the macro code always thinks in terms of the "dummy lane", and `trainingDrivenLane` and `trainingPolicyLane` resolve the real ones.

**Triggers use the same machine.** `requestTriggeredPlayback` enters `Playing` directly for a recorded slot. `requestTriggeredSequence` and `requestBufferedReversalSequence` play generated moves from the catalog in `generatedFrames_` (up to `kTriggeredSequenceCapacity`) and ignore `facingPolicy_`. All of these require `Idle`, so a trigger can never interrupt a recording.

#### Slots

Save states and macros have separate storage and one selector: a `static_assert` binds `kTrainingStateSlotCount` to `kTrainingMacroSlotCount`, both 8, and a next/previous edge updates both.

A state slot (`TrainingStateSlot`, `g_trainingStates`) is a whole-image copy of the same registered regions rollback uses (`g_rb`), plus FPU state and the recovery RNG. The gate refreshes a scratch snapshot (`g_trainingScratch`) before each step. If a load or reset fails its invariants, `trainingRestoreScratchOrFailClosed` restores the scratch, and if that fails too, training disables itself through `trainingFailMutation` rather than continue on a damaged image. A layout change marks every slot stale and `trainingLoadState` refuses it with `RejectedStaleLayout`.

Slots live only in memory. `trainingBeginReturnToCharacterSelect` calls `g_trainingMacros.reset()` and `trainingInvalidateStates(false)`, so leaving the match discards them. `selectSlot` refuses to change macro slots unless `Idle`.

#### Restore all

"Restore all" is not a stored flag. `trainingRestoreAllPresetActive` derives it: health, EX and guard gauge policies must all be `ResourceTrigger::WhenNeutral` targeting `TrainingRefillTarget::Both`. The F4 edge writes those three `ResourcePolicySettings` directly, and the menu checkbox sends one command per policy (`setRestorePreset`). The two cannot disagree because they share no separate state.

The policies fire on signals, evaluated each step:

| Signal | Condition | Used by |
|---|---|---|
| `neutralReady` | `evaluateFighterNeutral` clear for both fighters and `evaluateNeutralGlobals` clear for `kTrainingNeutralHoldSteps` (2) | EX, guard gauge |
| `exactIdleReady` | Both fighters in a free-movement action (`trainingFreeMovementAction`: idle, walk, crouch) for `kTrainingExactIdleHoldSteps` (11) | Health, EX, guard gauge |
| `resetAccepted` | A reset was accepted | All |

`resolveHealthPolicyFire` deliberately ignores `neutralReady`, so life never comes back on the short signal. "Restore full HP" is the same machinery with only the health policy on. The refills are idempotent while the signal holds, and `changed` only counts real changes. They run on every qualifying step, not once per combo, which is why the README says they do not wait for another combo.

Three related details:

- F4 is also level-triggered: holding it sets meter to full when the preset is on and zero when it is off (the `refillHeld` block in `trainingAdmitAfterNativeGate`). The edge toggles first, so the press that turns it on also fills.
- `g_trainingExRestoreTarget` pins the level EX restores to after you charge with F5. The menu shows "Restore EX to full again" to clear it, and turning the preset on with F4 clears it too; the menu checkbox does not.
- Slots 2 and 3 are never touched, because the game's round adjudication reads those fighters unchecked in its death path. `NeutralSlotOutOfRange` enforces this.

#### Hitbox and input viewers

Both viewers follow the same pattern: the game thread publishes a snapshot, the render thread reads it, and neither blocks the other.

**Hitboxes.** `HitboxDisplayMode` is `Off`, `Custom` or `Vanilla`. `trainingPublishHitboxSnapshot` builds a `HitboxSnapshotV1` and writes it to `TrainingHitboxPublisher`, a two-buffer, sequence-checked, word-atomic seqlock. The wrapper pulls it through the provider registered with `EFZDX9_RegisterOverlayProvider` and draws `OverlayCommandAbi` commands (`src/training/TrainingHitboxOverlayBridge.h`), capped at `kOverlayMaxCommands`. Each command carries owner and kind in its flags, which is how the per-player and per-box-type switches filter without new data. `Vanilla` instead sets the game's own display flag through `hitboxVanillaFlagValue`. Replay playback uses `Custom` under `g_replayHitboxOwner`.

**Inputs.** `TrainingInput::advance` output goes to `qinput::inputDisplay().publishStep` with mode `InputDisplayMode::Training`; `publishInputDisplayLatch` does the same with `ReplayPlayback`. The viewer therefore shows exactly the masks the simulation received, after SOCD cleaning and macro override, not the keys you pressed. `laneActiveMask` marks the driven lane. The DLL registers its provider through the wrapper's `EFZDX9_RegisterInputDisplayProvider`. `InputDisplayTypes.h` has its own API version so a training ABI change cannot break the replay viewer.

**Why the viewer reads the simulation.** The input viewer's contract is "the mask the simulation consumed", and every case where a player needs it is one where a device poll would be wrong: a macro recording routes P1's stream into the P2 lane, a macro or dummy policy drives a fighter nobody is touching, SOCD can delete a held direction, composition returns neutral while the rearm gate is not armed, and a replay has no device at all. ImGui's key state cannot serve either, because the wrapper forwards messages to ImGui only while its menu is open.

**The producer owns the rings.** Rendering runs at present cadence and the simulation at step cadence, and training's pause and frame step separate them completely. So `InputDisplayComposer` is the only thing allowed to fold a step, and the wrapper only reads. `inputDisplayLaneAppend` extends the newest entry when the mask repeats, because one row per frame would show about a quarter of a second of history, and it saturates at `kInputDisplayHoldCap`, since a wrapped count would read as a new press. The timeline is mode, generation and `drivenLane`, and a change to any of them discards the rings. Without the `drivenLane` clause, a macro record would re-anchor the human's P1 history onto the P2 strip, where it reads as the dummy's. A repeated or backward step serial is ignored. Equal serials on two reads mean nothing advanced, so a consumer must not flash new input.

**It fails by freezing.** Any `InputDisplayPublishResult` other than `Published` writes nothing and leaves the strip on its last good content. A failed seqlock read keeps the previous view. A frozen strip is a visible symptom, while a strip scrolling invented content looks like working software. `inputDisplayDirection` follows the same reasoning: an opposing pair cancels its own axis only, so it never renders an arrow for an input nobody made. `InputDisplayMode::None` is 0, so a zeroed frame or a provider that wrote nothing draws nothing. That default keeps the viewer off netplay and spectating, where no producer registers.

**One bridge, its own ABI.** `src/input/InputDisplayOverlayBridge.h` holds one thunk bound to the composer, not to a producer, so registration order between training and replay cannot be observed. It is a header because `qoh99_training` must stay Win32-free and because the inline function's static gives one registration across the program. The thunk runs on the presenting thread, so it touches only the seqlock. `kInputDisplayApiVersion` is separate from `kTrainingApiVersion`, because a training ABI break must not disable the replay viewer. `kInputDisplaySnapshotV1SizeTiers` only ever appends: the provider copies the largest released tier that fits the caller's buffer, and `inputDisplayReleaseOverlay` must run so the wrapper does not keep a D3D9 device provisioned for nothing.

### Replays

**What records.** `planRecording` builds a `RecordingPlan` from one `switch` on `cfg.mode`. Netplay follows `recordNetplay`, spectator sessions follow `saveSpectatorSessions`, local VS follows `recordLocalVs`. Training, `MODE_REPLAY`, probes, self-tests and VS CPU (`kVsRow1vCpu`) never record. All three toggles default on.

**How.** The DLL pushes canonical ticks through `ReplayRecordingCoordinatorV2` and `ReplayRecorderQueue` so the game thread never blocks on disk. `ReplayRecorderHostV3` in the caster reads them from a named pipe and `ReplayWriterV2` writes the file, finalized with `finalize`. The caster holds the writer so a crash in the game cannot lose what already arrived. A recording that never finalized shows up as a part file (`isPart`), hidden unless `showIncompleteReplays` is on. The state proofs in the file are structural. `ReplayPlaybackSessionV2` compares them on every frame: a state hash, lifecycle or dispatcher mismatch is counted by `noteDivergence`, playback runs on, and the result is `CompletedWithDivergence`. A wrong frame number, lane mask or input domain ends playback at once. Rematch games and files from an older state schema run as `BehaviourVerified`, which skips the state hashes and keeps the other checks.

**Browsing.** `replaysMenu` drives `ReplayBrowserModel`. Scans run in the background (`ReplaySnapshot`), and a rescan keeps the old list until the new one is complete. The battle log (`ReplayBattleLogFile.h`) is a separate append-only file under the replay root, joined to replays by game UUID, which is why renaming or moving a file does not lose its set. `folderPlaybackOrder` defines `P` (play a folder): direct children only, not trash, not part files, in recording-time order. The Trash and permanent delete are separate actions, and nothing is ever pruned automatically.

**Playback.** `runReplayPlayback` validates the path (`isSafeReplayPath`), starts `ReplayBrokerV2`, and launches the game with `MODE_REPLAY`. The compatibility check happens before launch: `replayRefusalMessage` explains incompatible files. `ReplayIpcWorker` receives the file over the pipe and `ReplayRuntime` feeds each recorded tick into the same native step that online play drives. `replaySubmitPresentationCommand` carries every key: the wrapper reads a key, submits one command with a generation number, and keeps no local state. Stale commands from a previous file are dropped.

Stepping backward uses `ReplaySeekCache`: checkpoints every 16 frames, doubling `stride_` as needed to stay within 256 MiB. A backward step restores the nearest checkpoint and runs forward silently.

#### Takeover

Takeover plays the replay up to a frame, then gives you one fighter while the other keeps following the recording. The default keys are in the README (F, G, Q, R, 9, 0). All are keyboard-only rows in `kModeControlDescriptors`.

`ReplayTakeoverController` (`src/replay/ReplayTakeoverController.h`) is the pure state machine. `GameHooks.cpp` does the native work.

```
Inactive --begin--> Countdown --40 frames--> Active
Active/Countdown --Retry or Switch--> RestorePending --ok--> Countdown
Active/Countdown --Exit--> RestorePending --ok--> Inactive
any failure --> Faulted
```

| Step | What happens |
|---|---|
| Press F or G | `PresentationCommand::TakeoverP1` / `TakeoverP2`. Allowed only when `takeoverAvailable`, not navigating, not headless, and the cursor is at record 2 or later and before the end. Speed resets to normal. |
| Next frame | `replayTakeoverNeedsSnapshot` is true. The hook starts local input (`replayTakeoverStartInputRuntime`, which uses your Player 1 profile whichever fighter you chose), then `replayTakeoverCaptureSnapshot` copies all regions plus audio state. If anything fails, `replayTakeoverCompleteSnapshot(false)` resets and marks takeover unavailable for this playback. |
| Countdown | `kReplayTakeoverCountdownFrames` (40) with the simulation held. Nothing diverges yet. |
| Active | Each frame: `replayTakeoverCaptureCompactInput`, `replayTakeoverBeginFrame`, `replayRunOneTick` with `takeover = true`, `replayTakeoverCommitFrame`. |
| R or Q | `requestRetry` or `requestExit` sets `RestorePending`. The next frame calls `replayTakeoverRestoreSnapshot`, then `replayTakeoverCompleteRestore`. Retry returns to `Countdown`; Exit returns to `Inactive` and frees the snapshot and input. Pressing the other fighter's key is a `requestSwitch`, also through restore. |

**The frame merge.** `ReplayTakeoverController::resolve` starts from the recorded tick's `actions`, then overwrites each lane that has a captured value in `slots_`. A slot holds both lanes as 16-bit values, with `kNoInput` meaning "use the recording".

**Delay.** `capture` writes your input to `elapsedFrames_ + delay_`, not the current frame. The first `delay_` frames therefore still follow the recording. Later captures fill any gap forward, so no frame is left as "follow the recording" once you have taken over. Delay is 0 to 9 (`kReplayTakeoverMaximumDelay`). The wrapper reads `TakeoverDelay` from `[Replay]` in `ddraw_wrapper.ini` once per generation and submits it as `QOH99_REPLAY_TAKEOVER_SET_DELAY`. A takeover exit before the snapshot is ready keeps the delay across its `reset`.

**Past the end.** Capacity is `prepare(endpoint + kReplayTakeoverContinuationFrames)`, capped by `kMaxTimelineFrames`. When `currentReplayFrame` passes the recording's end, `recorded` is null, `beyondEndpoint` is set and the other lane reads zero, using `takeoverFallbackLaneMask` from the file's last tick. So you can keep playing against a standing opponent.

**Invariants.**

- The branch is never written anywhere. The snapshot and `slots_` are memory only, and `Exit` restores the exact pre-branch image, so the replay's state proofs still hold afterward.
- If the branch reaches a lifecycle boundary (`rollbackEligible` false, or the round counter at its end value), the hook calls `replayTakeoverRequestSafetyExit` rather than let a branch walk into menus the recording does not describe.
- Failures go to `Faulted` with a `ReplayTakeoverFault`, published as `takeoverFault` in the transport status. `DrawReplayTakeoverHud` draws only the delay number in that phase, so the fault itself is not shown. `Exit` is still accepted from `Faulted`; `Retry` is not.
- Transient sound effects are stopped on restore (`replayTakeoverStopTransientSfx`), and audio has its own transaction (`beginReplayTakeover`, `restoreAfterReplayTakeover`).

### Custom palettes, end to end

A palette file is JASC-PAL text, 16 colours, one file per colour slot: `palettes\<RESOURCE>_<n>.pal`, n from 1 to 4. The stem comes from `kPaletteResourceNames`. In memory the wrapper holds 4 variants of 16 BGRX entries, `kPaletteTableBytes` = 256, and entry 0 of every variant must be black because it is the transparent key (`PaletteValidateTable`).

The authority rule is one bitmask. `paletteOwnershipTick` in `GameHooks.cpp` calls `EFZDDRAW_SetPaletteOwnership` with the slots this machine may load from disk:

| Mode | Mask | Meaning |
|---|---|---|
| Offline | `0xF` | All slots from local files |
| Netplay | Local lane only | Your files for you; the other slot only over the wire |
| Spectator, replay | `0` | Nothing local; everything comes from the stream or file |

So a viewer's own palette folder can never change what a match looks like.

1. **Selecting.** `paletteCustomConfirmTranslate`, on the `hkInputRead` path, adds the confirm bit to a D press only at the colour stage, so D confirms as well as arms. `PaletteAutoCharSelectTick` in the wrapper sees D in the selector's button ring (`PaletteSelectorSawD`) and sets `armedVariant`. Offline, it first checks the file exists (`PaletteVariantFileExists`); no file means the normal colours and normal colour-slot rules.
2. **Mirror rule.** `PaletteColourStepInstallOnce` hooks the game's colour step so an armed seat stops counting as a colour conflict (`PaletteArmedSeatWearsCustom`). The hook checks a code signature (`kQohColourStepSig`) before installing.
3. **Loading.** `PaletteAutoMatchTick` captures the game's own table on the first filled frame (`PaletteTableLooksAuthored`) as `defaultTable`. It then loads the armed file, only if the slot is owned, and calls `PaletteSubmit`. A pending entry is applied into the game's table and `PalettePublishSlot` updates the remap. If the game reloads its table on a round or rematch, the tick re-applies the custom one.
4. **Online.** `EFZDDRAW_QueryPalette` answers only for a slot with `haveCustom`. The DLL sends a `qnetplay::wire::PalettePayload` once per `g_netEpoch` through `NetplayCore::submitPalette`. On receipt it drops anything claiming your own side and anything from another epoch, then `EFZDDRAW_SubmitPalette` runs the same validation as a local file. A refused palette costs colours, not the match.
5. **Spectators.** The host publishes a chained `SetEventKind::PaletteSelected` event carrying one variant's 64 bytes (`kSetPaletteEntryBytes`). `spectatorPaletteViewerTick` applies it through `paletteSubmitVariantToWrapper`. Because it is on the event chain, a viewer joining late gets it too.
6. **Replays.** `paletteRecordingTick` writes a `ReplayPaletteV2` (`ChunkKindV2::Palette`). At playback the prebuffer feeds `paletteHoldFromReplay`, and `paletteReplayApplyTick` submits it once the wrapper can accept it.
7. **Toggling.** `PaletteToggleVirtualKey` reads `PaletteToggleKey` from `[Overlay]`, default Tab, polled by `GetAsyncKeyState`, so it is not a Controls-page row. `PaletteAutoToggle` skips slots you own online; offline it toggles every slot with a custom palette. It refuses an entirely black table, which would draw a silhouette. It changes only the local display.
8. **Editing.** The Palette tab (`kTrainingPagePalette`) edits the game's table directly, so the change is live. Undo is per slot (`PaletteUndoRecord`, `PaletteStudioUndo`, `PaletteStudioRedo`), and `PaletteUndoExternalSync` keeps history consistent when a table changes for another reason. Save and load go through `PaletteSaveVariant` and `PaletteLoadVariant`. The editor offers palette selection only when `g_paletteLocalSlotMask` is `0xF`. Online the channel is the D press and Tab, nothing more.

`PaletteAutoLoad` (`[Overlay]`, default 1) turns the whole feature off; palettes still arrive but are not drawn, and the log says so.

**How the game draws colour.** Character sprites are 4-bit: every pixel in a `.CHR` file is an index from 0 to 15, and the colour a player picked is applied by offsetting that index. Each fighter slot has a 256-byte table holding four 16-colour variants end to end, and the fighter's variant byte holds 16 times the variant. The game runs in 16-bit RGB565, so neither the presenter's `params.palette` nor a DirectDraw palette has any effect on it. The sprite blitter reads a per-player 256-entry colour LUT that the game's own converter builds from the table. An edit to a table therefore stays invisible until that LUT is rebuilt, and `PalettePublishSlot` does this by calling the game's converter (`kQoh99PaletteToLut`) for the whole table. That call runs on the game thread, inside the present.

**What is safe to touch.** The tables and LUTs sit outside every rollback region in `qoh99_re/model/qoh99_manifest.toml`, so writing them cannot affect a hash or a savestate, and palettes can arrive at any moment in a netplay session. The variant byte is different. It lies inside the fighter record, which is gameplay state, so changing it would desync a match. That is why selecting a variant in the editor needs full local ownership.

**Reading D at the colour stage.** The game's colour-stage handler masks the selector's button ring with `& 7`, so D alone (8) does nothing there, while A with D (9) masks to the native confirm. The game then takes its own confirm path with its own bookkeeping, and the wrapper only has to observe that D came along. Do not write into the ring to fake a confirm: the tick runs at Present, after the frame's selector logic has read the ring, so an injected entry is always a frame late. Two details follow from running at Present:

- The ring gains an entry on every change, so a press and release can both land inside one displayed frame. `PaletteSelectorSawD` walks every entry since the last look instead of sampling the head.
- The native handler consumes the confirm in the frame the press arrives, so the slot has usually left substate 2 before the tick runs. `previousConfirm` keeps that edge visible.

Substate 1 is excluded because A+D at the character pick already means the alternate form. Falling back below substate 2 drops `armedVariant`, so a custom palette cannot carry over to a character the player then changed.

**When a table can be trusted.** The match scene becomes current before the game fills its tables, and `PaletteAutoMatchTick` runs on every present, so its first looks land in that gap. Three rules protect it:

- `PaletteAutoTick` zeroes all four tables on entering character select. They would otherwise still hold the previous match's colours, pass `PaletteTableLooksAuthored`, and be captured as this match's defaults, after which the real fill would paint over the custom palette. The write is invisible, because the blitter reads the LUTs and not the tables.
- A table is "authored" only if an entry other than the transparent key is non-black, in any variant. An unfilled table captured as the default would turn the fighter into a silhouette on toggle, so `PaletteAutoToggle` refuses an all-black source as a last defence.
- Captured defaults are keyed on the character byte, not on passing through character select, because a rematch can swap a fighter without that scene appearing.

A rematch reload refills the table and rebuilds the LUT without showing character select. It is recognised exactly: the table is byte-equal to the captured default while `usingCustom` is set. The custom table is then re-applied, so neither side depends on when a peer's copy arrives. A file that cannot be loaded restores the authored colours explicitly, because otherwise D would mean "whatever an earlier match left on this slot".

**Threads.** `PaletteSubmit` can be called from the render thread (local files), the netplay thread (a peer) or the IPC worker (a replay), and `PaletteAutoMatchTick` always consumes on the render thread. `g_palettePendingMutex` guards the copy so a half-written table cannot be painted. Because everything is queued, arrival order does not matter: a palette that lands before the table is filled waits for it. A 64-byte submission (`variantOnly`) is spliced into the authored table at consume time, the same construction the local loader uses, so a spectator sees what a local file would have shown.

**Seeing an edit while paused.** A held frame never reaches the game's blit, so a rebuilt LUT changes nothing on screen until the next simulated frame. Held frames are still re-presented, so `PaletteRebuildRemap` builds a 65536-entry RGB565 table mapping the colours last drawn to the colours now wanted, and the presenter applies it per pixel. The reference is `g_paletteDrawnLut`, which `PaletteNoteCanonicalFrame` refreshes on every canonical frame, because the colours on screen are the ones last drawn and not the ones present when the editor opened. The limit is that a framebuffer records colours, not their source: a stage pixel with the same RGB565 value changes too, until the game next draws a real frame.

**Undo.** One undo step is one gesture, so a held key or a picker drag extends the open step. Palettes applied by the system (the D loader, Tab, a rematch re-apply) go through `PaletteUndoExternalSync`, which moves the baseline without recording, so undo never reverts one of those as a side effect.

### The settings map

The tables below say which screen owns which store. Every caster row reads and writes through `Settings` (`load`, `save`) in `src/client/Main.cpp` unless it says otherwise. For what each option does for a player, use the README.

| Screen | Function | File | Keys or fields |
|---|---|---|---|
| Settings row "Language" | `settingsMenu` | `qoh99caster.ini` | `language` |
| Player and files | `settingsPlayerAndFilesMenu` | `qoh99caster.ini` | `nickname`, `nicknameUtf8Hex`, `gamePath`, `dllPath` |
| Connection | `settingsConnectionMenu` | `qoh99caster.ini` | `port`. The punch-service row is read-only |
| Audio | `settingsAudioMenu` | `qoh99caster.ini` | `musicVolume`, `soundVolume` |
| In-game display and keys | `settingsInGameDisplayMenu` | `qoh99caster.ini` | `hideNativeScore`, `hideNativeTimer` |
| Game config | `gamecfg_ui::gameConfigMenu` | `System\QOHcnf.key` | The game's own format |
| Extra functions | `fcmd_ui::extraFunctionsMenu` | `LocalConfig\FCommand.txt` | Native developer options; `disableExtraFunctions` in the ini |
| Let people watch your matches | `spectatorServingMenu` | `qoh99caster.ini` | `spectatorAdmission`, `spectatorMaxViewers`, `spectatorPort` |
| Replays and storage | `replayStorageMenu` | `qoh99caster.ini` | `recordNetplay`, `recordLocalVs`, `saveSpectatorSessions`, `replayRoot`, `showIncompleteReplays`, `showTrashedReplays`, `showReplayNetworkDetails`, `replayFlushSeconds`, `replayStorageWarnMb`, `replayDefaultAudio`, `battleLog`, `replayPauseAtStart` |
| Controls | `controls_ui::controlsMenu` | `qoh99caster-controls.json` | Fighter profiles and mode controls |
| Join and Spectate address prompts | `interactive` | `qoh99caster.ini` | `joinIp`, `joinPort` |

No caster screen owns these:

| Store | Written by | Holds |
|---|---|---|
| `ddraw_wrapper.ini` | The wrapper (`OverlayIniSet`, `HudIniPath`); the caster's Controls page replaces `[Hotkeys]` and its launch writes `[Overlay]` `Language` | Window, renderer, HUD; `[Overlay]` `MenuStyle`, `PaletteAutoLoad`, `PaletteToggleKey`, `InputViewerSides`; `[Replay]` `TakeoverDelay` |
| Training menu state | The DLL, in memory | Dummy, guard, recovery, trigger and resource policies; gone when the training session ends |
| Environment | You | `QOH99_RELAY_OVERRIDE`, `QOH99_PUNCH_RELAY_OVERRIDE`, `QOH99_PUBLIC_IP_OVERRIDE`, `QOH99_LOG`, `QOH99_WRAPPER_LOG` |

Two rules follow from this split. The running game cannot change caster settings, because configuration flows one way at launch and only the wrapper's own ini is written live. And to add a setting, decide first who needs it: if the DLL does, it must go through `IpcConfig` in `src/ipc/IpcConfig.h`, which caster and DLL must agree on, not just a menu row.

## The frame-hook core

The game has no rollback support. It believes it is running a local two-player match. `src/GameHooks.cpp` takes over the native per-frame dispatcher, decides each time it is called whether a tick may run, and supplies everything a tick could otherwise read from the outside world: input, time, and the pacing gate. Three smaller parts carry the rest.

| Part | Owns | Knows about game memory? |
|---|---|---|
| `src/GameHooks.cpp` | Hooks, the per-frame driver, the frame metadata ring | Yes |
| `qnetplay::RollbackSession` (`src/netplay/RollbackSession.*`) | Input timelines, prediction, what to do next | No. It only sees frame numbers, input words and hashes |
| `qoh99::RollbackManager` (`src/RollbackManager.*`) | The savestate pool, restore, canonical hashing | Yes, but nothing about the network |
| `src/game/DPlayTransport.*` | The socket side of the link, reached through the `net*` functions in `qgame` | No |

The session being pure is what makes it testable. It answers "stall, advance or roll back" and never touches the game.

### Capture: what is hooked and why

`hkFrameDispatch` replaces the match dispatcher. `hkScene1Dispatch` and `hkScene3Dispatch` replace the menu and character-select dispatchers. All three route to `doNetplay` when `g_mode` is `RunMode::Netplay`, passing their own trampoline as `orig`, so the driver is one function regardless of scene. The native loop treats a return of 1 as "nothing happened, keep looping" in all three scenes, and that value is `kNetHoldFrameRet`. Returning it without calling `orig` is how a frame is held: the game loop spins and no simulation runs.

`bindFrameDispatchThread` pins the first dispatching thread. A dispatch from any other thread during a session is terminal.

The other hooks remove inputs to the simulation that the two machines would not share.

| Native input | Hook | During an owned tick |
|---|---|---|
| Logical input | `hkInputRead` / `hkInputReadResolved` | Returns `g_netInject[player]`, the canonical word for this frame |
| Keyboard state poll | `hkPoll` | Zeroes the key state unless `g_netPhysicalPoll` is set, which is only true around our own capture |
| Wall clock | `hkTimeGetTime`, `hkGetTickCount`, `hkQueryPerformanceCounter` | `netSyntheticMs`, derived only from the frame number at 60.60 fps, on the dispatch thread |
| Frame pacing bracket | The three pace hooks tracked by `g_paceBracketHooked` | While `g_netGateOwnsTick` is set, the gate answers yes once per tick, counted by `g_freezeGateCalls` |
| Sound status and playback | `hkSoundStatus`, `hkSoundDirectPlay` and neighbours | Status is answered from `virtualSoundStatus`, because the game's logic reads it. Plays update the virtual state and are captured as audio intent |
| Config save, command file parse | `hkConfigSave`, `hkFCommandParse` | `hkFCommandParse` applies session policy to parsed settings. `hkConfigSave` restores the player's own bindings with `netRestoreOfflineInputBindings` before saving |

`doNetplay` rechecks on every call that these hooks are still in place (`g_readHooked`, `g_pollHooked`, `g_paceBracketHooked`, `g_clockHooked`, `g_configSaveHooked`, `g_fCommandPolicyHooked`, `playbackSideEffectHooksReady`). If one is missing, the session ends. A session never continues with a hole in determinism.

The game also has to stay in local mode: `kNetIsNetworkAddr` must read zero, since both slots are fed through the two-player path. The local scheduler settings at `kQohNetSchedulerNormalizedOffsets` are forced to zero by `netNormalizeLocalSchedulerConfig` and then only verified by `netLocalSchedulerConfigIsNormalized`.

One native tick runs inside `netRunOwnedTick`, in this order:

1. Service any DirectBoot step, which owns the tick and forces neutral input when active.
2. Load `g_netInject` from the canonical inputs, then apply the post-match carry filter (`g_netWinscreenCarry`), which stops a held confirm leaking into the next screen.
3. Set `g_netCanonicalPoll` and `g_replayActive`, and open an audio tick with a disposition (`AudioTickDisposition`). Corrections and hidden ticks are journaled differently from visible ones.
4. Set the presentation disposition (`PresentationDisposition`), so the renderer knows whether this tick draws.
5. `netClockBegin(frame)`, set `g_netGateOwnsTick`, and call `orig` through `netInvokeOwnedTickWithSeh`.
6. Clear everything again. A structured exception from the game becomes a terminal error, not a crash mid-frame.

**The pace gate never answers "not yet" forever.** The native tick busy-waits on its limiter from inside the tick, so a permanently shut gate would hang the game inside `orig` and the driver would never run again. Stalling is therefore done only by not entering the tick. While `g_netGateOwnsTick` is set, `hkPace4704E0` answers yes to the first query and no to every later one, because the tail of the native tick queries the gate again for its own catch-up loop, and an always-open gate would step part of a frame twice. `hkPace470510` answers zero for the same reason: the native "are we behind?" check otherwise runs an extra partial fighter sub-step without advancing the frame counter, driven only by wall time.

**System functions are intercepted in the game's import table, never in their own code.** `patchQohImport` rewrites the game's own import slots for the three clocks and the joystick calls. The bytes of `kernel32` and `winmm` belong to the compatibility layer under Wine and may be relay thunks shared with every module in the process. Import patching also leaves this DLL, the wrapper and SDL on the real clock. An unresolvable slot fails closed with an `[iat]` log line and never falls back to a code detour.

**The logical read must be a pure function of the frame.** The native code reads an input slot several times per frame, so every branch of `hkInputReadResolved` answers from a value fixed for the whole tick. The native consumer wants the expanded mask (directions at bits 16 to 19), while `.q99r` stores the compact form (directions at bits 8 to 11), so stored values must pass through `unpackLogical`. Buttons happen to line up in both forms, so a missed conversion plays buttons and silently drops every direction.

**The custom-palette confirm has to live on this path.** Natively, the colour stage masks the button ring with 7, so A confirms, B backs out and D alone does nothing. `paletteCustomConfirmTranslate` adds the A bit to a D press, and the handler's own confirm path takes it. Only the read can do this: the wrapper ticks at Present, after the frame's selector logic has read the ring. It is deterministic because its inputs are the resolved mask and native words both peers hold in exact-input character select, and it is idempotent, so a recorded stream replays unchanged. Its gate copies the handler's preconditions exactly (selector live, that player on the colour stage) and adds none, and a refusal logs `[palette]` with both values. Training widens it for lane 0 only, since one person drives both selectors there. It cannot widen in a local two-player match: the native launch gate ORs every player's ring, so a confirm bit minted into a finished selector would start the match while the other player is still choosing.

**Per-build signatures.** Most hook targets have identical bytes in both builds. Where the signature window covers a relative `call` whose callee did not move but whose call site did, the bytes differ although the instruction means the same thing. `seSelectPerBuildSignature` picks between windows written out in full from the real executables, and `src/netplay/tests/test_build_signatures.cpp` checks each against both images. A missing variant shows up only in modes that verify that target, so offline play working proves nothing about netplay for a build.

**The native loop has no limiter of its own.** The outer loop re-enters the dispatcher hundreds of thousands of times per second on a modern CPU, and the native offline limiter is a polling gate inside the match tick. A dispatcher entry is therefore not a frame. Anything periodic, logging above all, has to gate on wall clock and never on an entry count, or a flushed log line becomes thousands per second. A hold that returned without yielding would burn a whole core, so every hold path sleeps, and a failed rollback request is terminal, since `nextAction` would otherwise return the same `Rollback` forever.

**Hooks install as one transaction.** `installHooks` queues every MinHook detour, patches the game's clock import slots (`netInstallQohClockImportPatches`), and commits with one `MH_ApplyQueued`, while the main thread is still suspended by the launcher and before any worker thread exists. That single commit is the only global thread freeze of startup. Failure runs `rollbackHookTransaction`, which removes every hook and restores every import slot, so the game resumes on a byte-identical image. Only the SelfTest run mode installs anything later. The import patches are not code detours and take no part in MinHook's queue. Replay needs them as well as netplay, because the game's per-frame effect emitters read the wall clock.

**Hook signatures are per build only where they must be.** The hook targets share addresses across the two builds. The few byte differences are call displacements, so a signature differs per build only if it is long enough to include one. The signature check reports which site failed, with expected and found bytes. A netplay-only site can fail while offline runs on the same build succeed, because those sites are verified only in deterministic timeline mode.

**The DLL is pinned and never unhooks.** MinHook teardown cannot be made race-free while game, renderer and UDP threads can enter a trampoline, and waiting for them in `DLL_PROCESS_DETACH` would deadlock on the loader lock. The operating system reclaims everything at exit.

**The input wrap is required by more than netplay.** `hkInputRead` is how a controller profile overrides an offline player's input, how replay installs stored masks for the whole canonical tick, and how an unattended harness boot's confirm presses reach the native selector rings. Those presses are needed from the first character select, which is why that path installs the wrap early.

**Two ownership questions look alike.** `ownsNetplayPacing` asks who drives presentation timing, and it includes replay playback. `ownsNetworkPacing` is the strict subset where a remote peer depends on this thread. Use the second when the question is whether something would stall a peer.

**Simulation-gating settings.** `applyNetplayFCommandDefaults` lists only values that can change the simulation, pacing, native network mode or developer state. Presentation settings stay untouched, with one exception that looks like presentation and is not: `kFullColorDisplayFlag`, bit 0 of the profile display flags. In the native code that bit decides whether whole effect paths run and which background asset set loads, and its value is copied into a gameplay-hashed region at match launch. Two peers that disagreed on it would differ in the hash, so it is forced on. The neighbouring NoSound and Vsync bits are true presentation and are left alone.

**Which sounds matter.** Only the ids in `kSimulationStatusSoundIds` have a status that the native control flow reads: 2, 4, 6 and 8 are the character-select voice gates, 162 is the round announcement, and 166 to 171 are the round-result announcements. The rule is that nothing about a sound file reaches the simulation, neither its length nor its presence. Lengths are frozen in `canonicalStatusSoundFrames` at the stock asset values, which keeps announcements paced as authored. The character voices vary per character and take `kFallbackSoundDurationFrames`. Presence is handled by `hkSoundLoad`, because the native code arms a voice gate only when the sound loaded, so a missing `.wav` would otherwise leave character select earlier on one machine. `waveChunkNextOffset` accepts one quirk of the stock files: some character WAVs omit the RIFF pad byte when an odd-sized data chunk ends at end of file.

**Why the game must not be told it is networked.** Both slots are read through the local path in menus, character select and match alike, so the game's DirectPlay code never runs. Do not write the native network-mode globals to "help" it. The native boot-batch gate tests only some of them, can fire in local mode, and then dereferences a DirectPlay object that was never created.

### Session start

Until `g_netRunning` is set, `doNetplay` is a rendezvous, not a simulation. Once, at the DirectBoot start barrier, it snapshots `g_netLocalReady`: the three RNG words, a `netBootstrapFingerprint`, and the match rules from `netReadActiveMatchConfig`. It resets the FP contract first, so the fingerprint is taken under the agreed environment. It keeps resubmitting that record with `netSubmitLocalReady` until `netReadyComplete` and `netGetRemoteReady` succeed.

When both records are present:

- A differing `bootstrapHash` is terminal.
- The record from P1 (`g_netLocalCtrl == 0` picks the local one) is authoritative. Its seeds are written to memory, and its rules applied with `netApplyAuthoritativeMatchConfig`.
- `netInitializeRollbackSession` builds the session with `firstFrame = 0`, `historyCapacity = kNetMetaSlots` (4096) and `g_netEpoch = 1`.

Frame numbers then advance on both machines from zero, and every later comparison is by frame number.

**Why the rendezvous stops at the main-menu barrier.** The render callback cannot advance the network-driven boot, so `doNetplay` runs neutral local initialization only as far as the DirectBoot start barrier. Both peers are then at the same semantic starting state before seeds are exchanged. The recording's deterministic start and the spectator `GameStart` are captured at this one seam from the same agreed triple and phase tag, so they cannot describe two different sessions.

### The per-frame order in `doNetplay`

The order is load-bearing. Each step can end the frame by returning `kNetHoldFrameRet`.

| # | Step | Function | Why here |
|---|---|---|---|
| 1 | Terminal check | `g_netHalted` → `netPostTerminalHold` | A halted session never runs another tick |
| 2 | Clock fault check | `g_netPacingClockFault` | Latched by the pacing owner when the real clock fails |
| 3 | Preconditions | Binding pointer, local mode, scheduler config, hook presence | Everything below assumes them |
| 4 | Link check | `netCheckLink(false)` | Picks up a peer's terminal message before doing work |
| 5 | Local controls | `netPollGameplayDelayHotkeys` | Requests delay changes, which apply at step 10. Rollback window changes apply here |
| 6 | Rendezvous | The block guarded by `g_netRunning` | See above |
| 7 | Ingest | `netSetSimulationFrame`, `netDrainRemoteInputs` | Remote input enters the session in frame order here, and only here |
| 8 | Liveness | `netPeerAlive` | Packets stopped arriving (`livenessTimeoutMs`, default 10 s) |
| 9 | Test barrier | `g_netTestPauseRequest` | Harness only |
| 10 | Apply delay change | `netApplyPhaseInputDelay` | Must precede capture, because capture computes its target from the delay |
| 11 | Delay drain hold | `g_netDelayDrain` | See [Delay and rollback changes](#delay-and-rollback-changes) |
| 12 | Session events | `netPumpSessionEvents` | True means hold this frame |
| 13 | Layout refresh | `reregisterFrameData` | A gameplay segment's layout changed, so rebuild before saving into it |
| 14 | Capture | `captureLocalInput`, `netFlushLocalSubmissions` | One sample per source frame, sent immediately |
| 15 | Decide | `nextAction`, or `nextQuiesceDrainAction` during a drain | The session's verdict |
| 16 | Slew | `netRegulatePaceSlew` | Small clock adjustment from prediction-depth balance |
| 17 | Rollback | `netReplayCorrection` | Takes the whole opportunity, then holds |
| 18 | Stall | The `ActionKind::Stall` branch | Progress deadline, hash check, optional early input production |
| 19 | Schedule | `planSchedule` | How many forward ticks to run: one, plus bounded catch-up |
| 20 | Advance | `netAdvanceOneCanonicalFrame` in a loop | Save, tick, commit, verify |

Capture happens before the decision, so a frame whose input was captured this call can advance this call. The capture target is `sourceFrame + inputDelayFrames`, and the same word goes to the peer through `netFlushLocalSubmissions`. Because the frame number travels with the word, the peer needs no knowledge of our delay setting.

### Advancing one frame

`netAdvanceOneCanonicalFrame` does different work depending on `rollbackEligible`, which is true only for `QohPhase::RoundActive` and `QohPhase::RoundHitstopOrFreeze` (`SceneSystem::policyFor`).

**Outside rollback gameplay** (menus, character select, intros, post-round) the game runs in exact lockstep. A frame with a predicted remote input, or without `netRemoteActualThrough(frame)`, holds with `HoldCause::LifecycleBoundary`. No savestate is taken and `preHash` stays zero. These phases allocate and free native resources, so memory restore could not undo them.

**In rollback gameplay**, on the first frame of a segment:

1. `ensureRollback` registers dynamic regions and builds the pool.
2. `netCanonicalizeHumanGameStartTransients` clears per-machine start noise.
3. `g_rb.validateLive` checks the live state is one the manager can represent.
4. `netResetFpuContract` sets the FP environment.
5. `netLayoutBarrierReady` exchanges a digest of hashed state, phase and FP environment with the peer. Virtual audio and the audio manifest travel beside it as diagnostics and do not join the digest. A digest mismatch is terminal and names the differing component. Waiting is bounded at 30 s.

Then, every frame:

1. `netSavePreFrame` saves into slot `frame % kPoolSlots`, captures FP state, and hashes both. The slot is tagged `(layoutGeneration, epoch, frame, phase)`.
2. `netMetaBeforeTick` builds the frame's record of RNG words, effective life and proof data. It is stored in `g_netMeta[frame % kNetMetaSlots]` only after `commitAdvance` accepts the frame.
3. `netRunOwnedTick` runs the game.
4. `commitAdvance` re-derives the inputs and refuses if they differ from what was just used. Rejection restores the pre-frame state and is terminal.
5. `netPublishAndVerifyConfirmed` drains confirmed frames.

The exact-input rule also covers the end of a round while still inside rollback gameplay. `netLifecycleTransitionRequiresExactInput` (`lifecycleTransitionRequiresExactInput`) is true when the round counter is 5, or the result state is 8 with the stage timer at 1 or less. The next tick there could leave gameplay and free native objects, so it waits for real input. If a speculative tick leaves gameplay anyway, the guard has missed a transition: the code fails closed with "speculative visible tick escaped exact lifecycle guard" and does not attempt to undo it.

**Frame preparation belongs to the boundary between ticks.** `netPrepareNextCanonicalBoundary` applies the next frame's preparation right after a tick, so the post-state proof of frame N equals the pre-state of frame N+1. Forward play, correction replay and takeover branches go through this one seam. The operations must be idempotent, because the same preparation may run again before the next pre-state capture.

`netNormalizeHumanMatchAiVm` shows why the placement matters. A human match never reinitializes the native AI script VM, so at a game boundary its globals still name the previous game's script allocations, which `validateLive` refuses. The VM region is hashed, and rollback layout setup is not frame-synchronized between peers, so zeroing it there would make one peer's bytes change frames before the other's. At the canonical seam every simulation writes the same bytes on the same frame. It runs only outside rollback gameplay. A selection that turns invalid during gameplay still fails through live validation.

`netCanonicalizeHumanGameStartTransients` clears a similar leak once per epoch. Character select feeds the same native command rings combat reads later, so their contents depend on how long the menu stayed open. The per-player AI state also retains frontend activity even though no AI owns a fighter. Both must start neutral at frame zero.

**Native lifecycle facts the driver depends on.** These describe the game's own code.

| State | Behaviour |
|---|---|
| Scene 2 / `matchSub` -1 | The match setup tick. Entered once per game, not between rounds. The simulation step counter does not move on this tick |
| Between rounds | Stays in scene 2 / `matchSub` 0. The owned tick that advances the round counter with tallies unchanged is the round boundary. The round result machine has several presentation substates, and its later ticks free character and frame data, even though `matchSub` is still 0 |
| Winner verdict | Ambiguous until the final `matchSub` 1 to 2 commit, so a round winner must not be read into `winnerSlot` earlier |
| Scene 3 to 2 commit | Resets the live selection cursors. A selector proof must come from before that commit, and an in-place rematch reuses the proof that staged the first game, since its roster cannot have changed |
| `kReplaySessionKnockoutLatchAddr` | One latch per side: this side has been knocked out since VS was entered from the main menu. Only the main-menu VS confirm clears it, so it survives rematches and a direct boot cannot reproduce a rematch's values |
| Formation row | 1 for two-fighter 1v1 rows, 2 for simultaneous 2v2, 0 when none has been applied. Test for team formation, not for non-zero |
| Selector routes and launch counter | Part of the combat snapshot. A real character select and a DirectBoot can reach the same bootstrap with different routes |

**The epoch rotates at the setup tick,** before the native setup tick and before replay capture, so setup and its adjacent frame zero belong to one epoch. Rotating at the first active tick would make a rematch's `GameStart` claim the old epoch while frame zero carries the new one.

**Stale proofs stay local.** A frame can become confirmed after a layout boundary, when a delayed final input from the old epoch arrives. Its inputs are still archived, but its hash is not submitted, because the transport rejects backward tag ordering behind the new epoch's reliable fingerprint. The log line reads "suppressed newly confirmed stale local hash".

**Dispositions.** `PresentationDisposition` answers two separate questions, drawing and pacing, and they do not line up.

| Disposition | Draws | Owns a pacing slot | Crosses the wrapper as |
|---|---|---|---|
| `Normal` | Yes | Yes | `Normal` |
| `SuppressHistorical` | No | No | `SuppressHistorical` |
| `HiddenNewForward` | No | No | `SuppressHistorical` |
| `VisibleFastForward` | Yes | No | `Normal` |

The wrapper compares the suppression value exactly and renders anything else, so `currentPresentationDisposition` must remap `HiddenNewForward`, and must let `VisibleFastForward` fall through. That fall-through is how a late-joining viewer sees its catch-up. A `Normal` tick consumes a slot, which both paces it and ends the batch, so a viewer drawing with `Normal` plays at 1x and never catches up. `isHiddenDisposition` asks "may this own a slot?". `isNonDrawingDisposition` asks "does this deliberately draw nothing?", and the presentation gate audit counts only those, because a fast-forward tick draws on purpose.

**The presentation gate tolerates a tick that draws nothing.** A non-drawing tick is expected to query the wrapper's disposition. Some legal lifecycle ticks never reach a draw, so a tick with no query is an error only if the wrapper has never answered at all: that is a stale or missing wrapper, and the session ends with "compatible ddraw presentation gate not active". Once any hidden tick has proven the gate, a queryless tick only increments `g_missingPresentationGateTicks`.

**Why the round outro is rollback gameplay.** The KO sequence runs inside the match substate, and a round end frees nothing: frame data is re-registered at a rematch, which is a new game. Every gameplay word the native sequencer writes is in a catalogued region. The only uncovered writes are banner descriptors, which are pure functions of the saved stage timer, recomputed each tick and handed straight to a draw call. So the outro predicts like normal play. Requiring exact input across it would cost one one-way latency per frame, roughly 250 frames at a time.

What stays exact is narrow. In native stage 8 the 60-tick countdown is ordinary, and destruction happens on the single tick where the timer has run out and both fighters' pose flags allow it. That one tick resets the sequencer, runs the fighter and assist loop, and advances the round. `lifecycleTransitionRequiresExactInput` therefore waits only there, which costs about one one-way latency per round, and at the round counter's match-end value. Save and restore with identical inputs is known to be reproducible across this tick. A misprediction that moves which frame commits is a different question, which is why the barrier remains. Do not remove it, and do not widen it back to the whole outro.

**Three input gates at phase boundaries.** All three exist because a held button looks like a new press after a screen change.

| Gate | Where it acts | What it prevents |
|---|---|---|
| `NetworkDirectBootInputGate` | Physical input, before it enters the timeline | Input sampled during DirectBoot is discarded and one neutral sample is required afterwards, so a held confirm cannot emerge later as a fresh edge. Automation supplying canonical input skips the wait |
| `WinscreenCarryGate` | Canonical words, per frame | One tap on the win screen confirming the prompt and then also picking a character. Stepped on injected input only, so players, corrections and spectators all reproduce it. The whole window is rollback-forbidden, so its state is never re-derived |
| `filterUnselectedCharacterSelectBack` | Canonical action words | B undoes a selection step, but cannot back out of the two-player selector to the mode menu. It runs at the execution seam, so its result is what replays and spectators store, and delay cannot queue a second Back against stale selection state |

### Prediction and rollback in `RollbackSession`

The session holds three rings of `historyCapacity` slots, indexed by `frame % capacity` and validated by the stored frame number: `_localInputs`, `_remoteInputs`, `_consumed`. Frame comparison is serial arithmetic (`frameAfter`, `frameBefore`).

Three frontiers define its state:

| Field | Meaning |
|---|---|
| `_frontier` | Next frame to simulate. `currentFrame` returns it |
| `_nextRemoteContiguous` | Oldest frame with no actual remote input. Everything before it is known |
| `_nextConfirmedFrame` | Oldest frame not yet published as confirmed. `oldestRestorableFrame` returns it |

**Prediction** (`predictRemote`) repeats the most recent actual remote input before the frame, or `neutralInput` if there is none. `speculativeFrames` is `_frontier - _nextRemoteContiguous`.

**`nextAction`** decides in strict priority:

1. A rollback already in progress: `Stall` with `ReplayInProgress`.
2. `_hasPendingMismatch`: `Rollback` from `_pendingMismatch` to `_frontier`.
3. No local input for this frame: `Stall` with `LocalInputMissing`.
4. Speculation at `maxRollbackFrames`: `Stall` with `PredictionLimit`. This applies even when the frontier's own remote input has arrived if an earlier one is missing, since a hole still has to be repairable.
5. Otherwise `Advance`.

**Detection** happens on arrival. `receiveRemoteInput` compares the actual word against what `_consumed` recorded for that frame. A difference calls `markMismatch`, which keeps the earliest frame. A different value for an already-held frame is `RemoteInputResult::Conflict`, and a value outside the fighter mask is `InvalidValue`. `netDrainRemoteInputs` turns both into `kNetProtocolViolation`. Peer values are rejected, never masked.

**Correction** in `netReplayCorrection`:

1. Check the origin's `NetFrameMeta` (valid, right frame, right epoch, non-zero hash), then `beginRollback`. That call rejects any depth above `maxRollbackFrames`.
2. `netRestorePreFrame` restores memory and FP state. The slot tag must match exactly.
3. For each frame up to the old frontier, verify the phase still matches the original record, then save again, re-tick with `PresentationDisposition::SuppressHistorical`, and `commitReplayFrame`. The new save overwrites the slot and the metadata, so hashes describe the corrected timeline.
4. `finishRollback`, then commit the audio correction. `AudioCorrectionScope` aborts it if anything returned early.

No forward frame runs in that same dispatch. If the replay reaches an exact-input boundary before its old frontier, `netHoldReplayBeforeBoundary` calls `finishRollbackBeforeBoundary`, which moves the frontier back and discards the speculative suffix (`netInvalidateDiscardedTimeline`). If a replay on verified input legitimately leaves gameplay, `finishRollbackAtBoundary` does the same at that exit. These are the only cases where `_frontier` moves backward.

**Local input is a single write.** `storeLocal` will not overwrite a stored frame with a different value, and `commitAdvance` / `commitReplayFrame` recompute expected inputs. Local capture is illegal during a replay (`captureLocalInput` refuses it), so a replay can never disagree with what the peer received.

**Why the pool is deep enough.** Saves live in `kPoolSlots` = `kMaxWindow + 1` = 31 slots. Speculation is capped at `maxRollbackFrames`, which cannot exceed `kMaximumGameplayRollback` (30). A `static_assert` beside `kPoolSlots` stops the build if those numbers drift. Every frame that could be a rollback origin therefore still has its slot, and the tag check catches any violation at runtime.

### The save and restore region model

**The catalog is data.** `qoh99_re/model/qoh99_manifest.toml` lists every byte range the project has classified: 97 `regions` plus 4 `pointer_children` (`cmd_p1` to `cmd_p4`, the fighters' command rings). `qoh99_re/tools/generate_model.py` turns it into `qoh99_re/include/qoh99/generated/GameplayRegions.hpp`. The build target `qoh99_re_manifest_check` runs the generator with `--check` so the two cannot silently diverge. Each row states a `relevance`, whether it is captured and restored, and whether it joins `gameplay_hash` or `render_hash`.

`saveRegions` in `src/qoh99_game.h` selects from `GameplayRegionCatalog::regions`:

| `relevance` | Rows | `RegionRole` | Saved | Restored | Gameplay hash |
|---|---|---|---|---|---|
| `DeterministicGameplay` | 45 | `GameplayRequired` | Yes | Yes | Yes |
| `RenderCosmetic` | 18 | `RenderCosmetic` | Yes | Yes | No |
| `WallClockPacing` | 5 | `WallClockPacing` | No | No | No |
| `ExternalResource` | 23 | `OSPointerUnsafe` | No | No | No |
| `Unknown` | 6 | `DebugOnly` | No | No | No |

Cosmetic regions, such as the arena blocks, are restored so a replayed frame draws correctly. They stay out of the hash because they may legitimately differ between machines. `safeToRestore` is a second gate in the restore path.

**Dynamic regions** cannot be in the static table, because they live on the heap and depend on the loaded characters. `ensureRollback` calls `registerFrameDataRegions` and `registerAiScriptRegions`, which add them through `addExtraPtrRegion` and `addExternalOwnerPtrRegion`. `registerImmutableAssetFingerprint` hashes loaded assets that are never saved, and `hashLive` mixes that value in first, so differing content produces a differing state hash. When gameplay layout changes (rematch setup, `gameplayLayoutConfigChanged`), `reregisterFrameData` drops all of it, invalidates the slots, and increments `g_netEpoch`.

**Snapshot layout** in a slot is all fixed regions concatenated, then for each pointer child a 4-byte presence marker followed by its bytes. `_totalSize` is that length. `init` refuses a pool above `maxSnapshotPoolBytes` (512 MB) and increments `_layoutGeneration`.

**Save and restore both validate first.**

- `snapshotTo` checks that every region is readable, the AI interpreter state passes `validateAiVm`, and every pointer child resolves to the same address it had at `init`. Otherwise it returns false and the session ends with "atomic pre-frame snapshot failed".
- `loadFromInternal` walks the source buffer, checking markers and children, and stages child copies before writing any live byte.
- `loadTaggedFromSlot` adds the `hasTaggedSlot` check. A restore of the wrong frame, phase, epoch or layout is impossible, not merely unlikely.

**Hashes are canonical, not raw.** The two machines have different heap addresses and different personal settings, so `hashSlot` and `hashLive` rewrite values before hashing:

| Value | Treatment |
|---|---|
| An actor's pointer into its frame-data table | `canonicalActorInteriorPointer`: tagged offset |
| AI script pointers | `canonicalScriptPointerForBank`: bank plus offset |
| Asset descriptor pointers | `descriptorIdentityFingerprint`: family, count, size and content |
| Config records | `canonicalizeQohPeerLocalConfigForHash`: zeroed except the four match rules in `QohMatchConfigProof` |
| Inactive AI VM stack | `hashAiVmCanonical`: only the slots up to the current depth are hashed |

`loadFromForeign` is the same restore for bytes from another process, used by spectator checkpoints. It relocates the actor and descriptor pointers to the local values. AI script pointers are not relocated: `validateAiVm` must accept the incoming VM state against the local script ranges. `kHashAlgorithmCurrent` selects the bulk hash. It is baked into recorded proofs and exchanged hashes, so it changes only together with the replay schema constants named beside it.

**Why frame data is a saved region.** The game rewrites frame durations and link fields in each fighter's heap frame-data buffer at runtime. On a deep correction the animation reset would read the edited value from the abandoned timeline, an off-by-one that shows up only at larger rollback depths. `kFdSlots` lists the owners. P1 and P2 are required, and registration retries until both are loaded with a plausible count. P3, P4 and the four helper slots are optional, validated per slot, because dual-character transforms mutate a helper's own buffer. A striker's slot can alias the primary's buffer, and saving it twice is harmless. Identical address and size pairs are folded at registration.

**Why AI scripts are saved.** The COM bank table is 20 fixed records. Each owns a script allocation whose length is stored in the original .Fob file, plus two sentinel bytes the loader appends. Script opcodes write gameplay variables back into that allocation, so all of it is rollback state and not an immutable resource. Human-versus-human loads no bank, and all twenty records stay in the loader's empty state: that is a complete layout, not a partial one. A bank appearing later is a lifetime boundary, caught by `aiScriptConfigChanged`. Two players may share one bank.

**Load-once roots are fingerprinted, not saved.** `kAssetDescriptorSlots` lists more roots than `kFdSlots`, including the effect template and the system Rina actor, which are shallow-copied into actor pools. Their resources affect actor identity, but no mutation has been shown, so they join compatibility without joining the snapshot. The effect template's sixth family is a real allocation with a count of zero, and it stays as an owner for lifetime checks.

**A loader transition re-mallocs everything.** All six descriptor families and the script children are reallocated together, so sizes or destinations frozen from the old generation would under-save or overwrite a recycled allocation. The check runs before every eligible save and restore and forces one atomic rebuild. Discarding the pool comes first, because an old tagged slot must never be read with a new stride or identity map. The pool cap exists because the DLL shares a 32-bit address space with the game, DirectX and character assets, and a pool allocation failure there means exhaustion or fragmentation, so the log reports the size requested and what the process could still offer.

**A restore's commit phase cannot fail.** Child destinations are taken from the snapshot's own owner bytes, not from the live pointers those bytes are about to replace, and each is proven writable before anything is copied. The activation words have already been checked against the recorded markers. The commit is then a sequence of copies with nothing left to check, so it can never return with fixed regions restored and children still holding the live frame, a state that never existed.

**Identity is structural, never discovered.** Hash exclusions and canonical words come only from the registered structure. Scanning for values inside allocator ranges would let two processes classify different numbers as addresses and produce different hash layouts. Actor pools initialize lazily, so a value scan at match entry would also miss pointer fields in a slot that activates later. Every actor field that matters is registered for every possible slot. Native facts behind this: the match loop updates 30 effect actors even though several spawn helpers search only the first 20, an unused actor slot is marked with 0xFF at `+0xAE` and keeps its stale contents, and a spawn copies a complete source before activation.

**`loadFromForeign` details.** Activation words keep their live values, because participation means non-zero and both processes agree on that. A pointer with an unknown identity keeps the snapshot's raw value, because the hash contract hashes it raw. A non-zero `unmappedActive` count on a verified session is a mapping gap worth investigating. An inactive team child keeps the snapshot's owner word: the game's static owner holds a non-null pointer this process never allocated, and it is never dereferenced while the marker is absent. The first hash verified after the restore is the actual proof.

**The config record.** The game keeps five installation-local configuration records. Their layout, as read from the game's code:

| Offset | Field | Treatment |
|---|---|---|
| +36, +40, +44, +48 | Timer mode, AI difficulty, life multiplier, rounds to win | `QohMatchConfigProof`, P1-authoritative at Ready |
| +52, +56 | Background and HUD render modes | Local |
| +60 | BGM backend | Local |
| +64, +84 to +96 | Frame pacing and clock mode, pacing counters | `kQohNetSchedulerNormalizedOffsets`, forced to zero. They alter how many simulation iterations run |
| +68 | Screen-effect mode | Local |
| +80 | Extra-function and system-pause bits. They also gate a live joystick poll | Cleared by `netApplyExtraFunctionPolicy` |
| +100 to +375 | Display, window and audio device modes, then the install path | Local |
| +380 to +763 | Physical input bindings | Local, never crosses to the peer |

Raw save and restore copy the whole record, since snapshots never leave the process except through `loadFromForeign`, which rewrites these pointers. Only the cross-process hash uses the canonical view. Netplay and replay force the four rules into the active profile, so the game's own copy can legitimately disagree. `matchRulesOverridden` turns off the active-versus-snapshot check while that is true, and a failure message from this check names the four values. It is not a state-hash divergence: both values come from this process's own memory.

**Why not save the whole data segment.** The game's writable data mixes simulation state with live OS objects. The `ExternalResource` rows, such as `ddraw_ptrs`, `dsound_buffers`, `window_primary` and `net_callback_ptrs`, hold handles and pointers that belong to the running process, and writing old values back over them freezes the game. That is why the save set is an explicit list, and why a span nobody has classified stays unsaved.

**Native facts behind some classifications.**

- `frame_counter` is `WallClockPacing` even though it counts native ticks. The game's FPS counter reads and resets it once per real second, and no simulation code reads it. A replay window can cross that one-second reset while the original run did not, so hashing or restoring it would report a desync where none exists. The driver only observes it (`kQohCompletedNativeTickCount`).
- The arena bookkeeping rows (`arena_mirror`, `arena_active`, `arena_head`, `arena_sel`, `arena_latch`) are one coupled set that depends on the buffer phase, and the simulation does not read them. They are saved and restored together, because restoring only some of them leaves the set inconsistent, and they stay out of the gameplay hash.
- `screen_fx_state` is a screen-effect sequencer. A config value can start it independently on each installation, and its only consumer emits render descriptors. It therefore has to be restorable for correct drawing, but it must never count as shared gameplay identity.
- `actor_clone_pool` holds 120 actor records. The native spawner (`kClonePoolSpawn`) clones a whole fighter record into a slot, and the actor stores its slot index, so saved fighters refer into this pool and the two must be restored together.

**Pointer children and the presence gate.** `cmd_p1` to `cmd_p4` are the command objects holding each fighter's direction and button rings, reached through a pointer inside the fighter record. Restore order follows from that: fixed regions first, so the owner's pointer holds its saved value, then the pointee.

The game assigns static command buffers to the P3 and P4 objects even in an ordinary 1v1, and their inactive bytes depend on the installation and on input history. A pointer that is merely non-null therefore proves nothing. `PtrRegion::activationPointerAddress` names the fighter's entry in the active-fighter table, and `pointerChildParticipates` lets the child take part only while that entry is non-null. `cmd_p1` and `cmd_p2` have no gate and are required. The same gate is checked against the live memory and against the copy inside the snapshot, so a slot whose saved team state disagrees with its presence marker fails the restore and shows up as a difference in the diff and hash paths.

The hash folds in the presence marker and the child's contents, never its address. The address is where the allocator put the object in this process, so including it would report a desync between two machines whose simulations agree, with no differing gameplay byte to find.

**Test-side copy.** `src/rollback/` (`SavestateProfile`, `SyncHash`, `RollbackRuntime`) builds the static library `qoh99_rollback`, and the shipped DLL does not use it. It runs the same rules against a `MemoryView`, without a game. `src/tests/production/test_production_parity.cpp` holds it to `saveRegions` and the production hash, so a rule changed in `src/RollbackManager.cpp` needs the matching change here. Two of its behaviours are deliberate:

- `SavestateProfile::restore` checks every child pointer before writing any pointee, because a partly written set of children would be corrupt state that the caller cannot tell from a clean failure. Fixed regions are already written at that point, so a false return means the state is unusable, and it is not a safe no-op.
- `SavestateProfile::diff` reports `child-count-mismatch` when two savestates disagree on how many children exist, so a structural difference cannot pass as "no difference".

`SyncHash` has three profiles. `GameplaySync` must show no difference across a save, tick, restore and replay pass. `RenderCosmetic` may legally differ during a replay. `FullDebugWide` covers everything captured and is for diagnostics only.

**Why the regions are carved so finely.** Neighbouring words in the game's data often belong to unrelated subsystems, so one coarse row would either hide gameplay from the hash or restore something unsafe. These are facts about the native game that decide the classification of each carve:

| Region | What it holds in the native game | Why it is classified this way |
|---|---|---|
| `block0`, `block1` | The per-frame render list, rebuilt from gameplay each frame | Restored so a replay starts consistently, but not hashed |
| `frame_counter` | An FPS statistic, reset each wall-clock second, with no simulation reader | Wall clock; hashing it would compare the machines' clocks |
| `frame_time_ring`, `frame_time_ring_index` | The frame-pacing history from `timeGetTime` | Wall clock; never saved, restored or hashed |
| `actor_clone_pool_tail` | Bytes past the last pool object, including a session-long frame counter | Restored but not hashed |
| `rng_selector` | Selects the gameplay or cosmetic RNG | Gameplay, though its neighbours are unrelated: the ring index, the drive cursor and a word no code reads |
| `opening_drive_cursor` | The drive-letter cursor used only to find the opening movie | Depends on each machine's drives, so it would reject otherwise identical peers |
| `window_primary`, `window_secondary` | Window, device-context, bitmap and palette handles plus OS client coordinates | Restoring a handle is unsafe, and even null versus present differs per process |
| The `renderer_surface_id_` rows | Renderer surface indices inside the match block | A display reset can recycle an index without a layout change, so restoring one is unsafe |
| `camera_subpixel` | The odd remainders of camera movement, accumulated across frames | Camera X feeds screen-edge push, so this is gameplay |
| `finish_fx_state` | The KO and finish sequencer's state, which gates normal fighter updates | Gameplay, despite sitting in a band of HUD data |
| `target_loop_fighter160_backup` | A per-target backup the collision loop writes before it runs and reads back after | A stale copy after a deep rollback writes a wrong value into a fighter |
| `round_banner_state`, `round_banner_audio_state` | Round counters, announcer state and the sequencer behind the round-call sounds | Read every frame, can draw gameplay RNG, and a wrong value skips or repeats round calls |
| `fighter_ptr_table`, `backup_fighter_ptrs` | Active and backup fighter pointers, swapped mid-match by the dual characters | Must be restored and hashed as a pair. The pointees are static objects, so raw values are safe |
| `freeze_requests_23` | Two of the arbiter's seven global freeze counters | Gameplay; carved out of the surrounding cosmetic bookkeeping so a difference in freeze length is visible |
| `window_adjacent_gameplay` | Another freeze counter, directly after a window object | Same reason |
| `ai_script_vm`, `script_bank_selection`, `ai_script_banks` | The CPU script interpreter | CPU fighters run inside the gameplay tick and draw the gameplay RNG |
| `match_aux_actor_pool_slots_05_29` | Shared auxiliary actors, including one stage's ambient spawns | The spawn branch schedules gameplay RNG draws, even though the actors it draws are visual |
| `rina_crash_actors` | Four live summon actors, one per player slot, ticked every frame | Their arming flag lives in the saved `dw_lo2`, so saving only the flag leaves a summon standing after a load. Their tick can reach the gameplay RNG. They have no attack record of their own |

The lesson generalizes: a word needs its classification from what reads it, not from what surrounds it. A gameplay word inside a `RenderCosmetic` row is restored correctly and still never hashed, so a divergence in it stays invisible until it reaches something that is hashed.

### FP environment

Native gameplay uses floating point, so the FP environment is part of game state. `NetFpuSlot` holds a 512-byte `fxsave` image per pool slot, along with `g_virtualAudio` and the memory slot's layout, epoch and frame tags.

| Function | Effect |
|---|---|
| `netResetFpuContract` | `fninit`, FCW `0x027F` (all exceptions masked, round-nearest, 53-bit), MXCSR `0x1F80`. Runs at the ready snapshot and at each gameplay segment start |
| `netCaptureFpu` | `fxsave` plus virtual audio, immediately after the memory save |
| `netRestoreFpu` | Restores both. `netRestorePreFrame` fails unless memory and FP restore both succeed |
| `netInvalidateFpu` | Clears every slot, on epoch change and session start |
| `netCanonicalFpuHash` | Hashes only FCW and MXCSR control bits, masking the sticky status bits |

The restore is the full image, so a replay sees identical registers. The hash covers only the control part, because status bits are not reliably equal and do not change results. A control-word change shows up in every later `preHash`.

`preHash` is therefore three things combined in `netTaggedPreHash`: the gameplay memory hash, the canonical FP hash, and canonical virtual audio (`canonicalVirtualAudioForHash`). Audio is included because sound status feeds back into game logic.

### Pause and hold semantics

"Held" has several distinct meanings, and they are kept apart on purpose.

| Cause | Where | Debt? | Bound |
|---|---|---|---|
| `StallReason::LocalInputMissing` | `nextAction` | No | Progress deadline |
| `StallReason::PredictionLimit` | `nextAction`, `planSchedule` | Yes, through `applyRebasedFrames` | Progress deadline |
| `HoldCause::LifecycleBoundary` | Exact-input checks | No | Liveness only |
| `AdapterHoldReason::DelayDrain` | `doNetplay` | No | `kRuntimeDelayDrainTimeoutMs` (5 s), then the change is cancelled |
| `AdapterHoldReason::LayoutBarrier` | `netLayoutBarrierReady` | No | 30 s |
| `AdapterHoldReason::SessionEvent` | `netPumpSessionEvents` | No | Liveness |
| `AdapterHoldReason::PresentationGate` | First gameplay frame | No | 2 s, then terminal |

All follow the same pattern: call `netObserveRollbackHealth` and `netPaceSemanticHold`, `Sleep(1)`, return `kNetHoldFrameRet`. Three properties follow.

- **A hold changes no simulation state.** `orig` does not run, and no frame number moves. Peers can hold for different lengths and stay in sync, because sync is by frame number, not by time.
- **A hold does not count as lateness.** `netPaceSemanticHold` calls `NetplayPacingDeadline::semanticHold`, which only refreshes its last sample. When the next `poll` finds the deadline more than one frame late, it rebases and reports `rebasedDebtFrames`. `applyRebasedFrames` keeps that debt only if the last hold was `HoldCause::PredictionLimit`, the one case where the delay is a real lag to recover.
- **The transport keeps running during a hold.** Ingest (step 7) and the link check precede the hold paths, and the stall branch still calls `netPublishAndVerifyConfirmed`, so the remote input that ends a stall is taken in on the next dispatch.

**Stalls have a deadline.** A frontier stuck on one frame warns after `kNetStallWarnUs` (8 s) and ends the session after `kNetStallAbortUs` (20 s) with `kNetTimeout`. This catches a peer that is alive on the wire but producing no input, which `netPeerAlive` cannot see.

**During a stall, input keeps being produced.** `netProduceLocalInputWhileStalled` calls `produceLocalInputAhead` so the peer does not starve in turn. It is limited by `inputProductionPermitted` and never fills the frontier's own delayed slot, which belongs to `captureLocalInput`.

**Catch-up is bounded.** `planSchedule` adds forward ticks from `g_netCadenceDebt` and from frame advantage over a fresh peer sample (`kNetPeerFrontierFreshMs`). Extras are capped three ways: `kNetSchedulePolicy` allows at most 3 extra with a deadband of 3; `safeForwardHeadroom` keeps them within the rollback window and is zero at exact boundaries and during the round intro's rotation; and `kNetCatchupBatchBudgetUs` (6 ms) truncates a batch. Only the last tick of a batch is visible (`playerCatchupTickVisibility`). `netRegulatePaceSlew` handles slower imbalance by adjusting the frame period in parts per million, and releases to zero on any unusable sample.

**Coordinated stops** go through the session event ladder in `netPumpSessionEvents` and `qinput::SessionEventTimeline`: `Quiesce`, `Report`, `Prepare`, `AtBoundary`, `Commit`, `CommitAck`. Local capture freezes (`g_sessionCaptureFrozen`), and each side reports its frontier and assigned input tail in a `QuiesceReport`. `hostChooseBoundary` picks the latest of those, refusing a drain above `kSessionEventMaxDrainFrames` or a boundary behind `oldestRestorable`. `fillLocalNeutralThrough` assigns neutral input up to the boundary, and both sides drain with `nextQuiesceDrainAction`, which does not require a fresh capture. The event applies only when `nextConfirmedFrame` equals the boundary, so both sides stop on the same confirmed frame. The kinds in use are `TerminateSession` and `ReturnToCharacterSelect`. `RematchCommit` and `RematchDecline` exist in the timeline and on the wire, but the prompt that requests them never opens. A rematch is decided in canonical input by `netCanonicalRematchTick`, with no hold and no session event.

`SessionEventKind::PauseSet` exists in the timeline and on the wire, and the driver honours a paused state through the same hold path. `netPumpSessionEvents` has no local control that requests it, and `netEvaluateOnlineModeControls` enables no pause action. The game's own pause keys are not available either: `nativeDeveloperKeyPolicy` returns `ForceDisabled` for netplay.

**Netplay rematch.** A rematch needs no hold and no wire traffic. `netCanonicalRematchTick` flips the native continue screen inside the owned tick and reads each side's answer from canonical input, and `netCanonicalRematchPostTick` arms the instant restart. Both peers therefore decide on the same frame from the same input.

**The rollback window is a budget.** Two things spend it. Transit commits about RTT/2 frames at delay 0, and every frame of lead over the peer spends another. What remains is the `headroom=` value in the log line that also carries transit and lead. At zero, every frame waits for the exact remote input. A high ping does not itself cause stalls; drift eats the budget that ping needs. Compare the peer's frontier only after correcting for its age, or transit is counted twice. Balancing does not use this estimate. It uses the prediction-depth report.

**Stall-time production has rules.** `netProducerFreshSample` reads worker snapshots through `netManagedPhysicalCapture`, so a press made during a stall survives. It returns false, and the producer repeats the held word, wherever the sample is not truly physical: unmanaged input (`g_fullInputPollFn` is the game's own poll, and an extra call would perturb native edge detection), the automatch test player, a rematch's committed taps, and DirectBoot's gate while it awaits neutral. A repeated word cannot create an edge, so that fallback is always legal. The producer runs at most once per canonical frame period, because both hold sites poll at 1 ms and would otherwise spend the whole lead budget in one frame. It depends on `netPollGameplayDelayHotkeys` advancing the input bundle near the top of the driver: if that pump moved below the hold sites, a stalled driver would re-read one cached bundle, which is the held word by another name. Starvation metrics must read `localInputProducedThrough`, the highest local input that exists, not the frontier plus delay.

**Rules for a catch-up batch.** Each of these keeps the opportunity's visible frame safe.

- `safeForwardHeadroom` is `ceiling - depth - 1`. It bounds the extra frames, so the 1 reserves the visible frame's own prediction slot.
- The time budget collapses the batch, moving `lastIndex` to the current iteration so that iteration draws. Visibility follows what ran, not what was planned, and comes from `playerCatchupTickVisibility` so no copy can drift from the tested function.
- A refusal mid-batch breaks out and never returns, because the tail holds the debt credit, the pacing fallback and the hold-cause reset. Returning there at an exact-input boundary would run a hidden frame per poll, with nothing drawn, for the whole KO window.
- A partial batch still credits the hidden frames it ran. Forfeiting them saturates the debt, which pins the batch size, which guarantees the next truncation.
- Catch-up with no debt was triggered only by the noisy advantage estimate, so it arms `g_netFaHoldCooldown` like a hold. Debt repayment stays unthrottled, since real missed frames bound it.
- A session-event drain forces single-frame opportunities. A batch could otherwise execute the frames either side of the boundary together and strand the peer past it, so a multi-frame plan there is terminal.

Frame advantage steers only rollback gameplay, because frontend lockstep legitimately oscillates by the frontend delay. The peer's frontier is projected forward by report age plus RTT/2, and that error leans toward "peer is ahead", which disables regulation and does not over-hold. `netRegulatePaceSlew` runs before the Rollback and Stall branches, because the turns where a lead has reached the ceiling are all stalls, and that is exactly when it has to act.

**Capture delay and exact input are separate gates.** `netApplyPhaseInputDelay` keys on `rollbackEligible`, so a round-end KO outro keeps the selected gameplay delay. Refusing to predict across the KO belongs to `netLifecycleTransitionRequiresExactInput`. Tying the delay to that second gate would raise input delay after every round.

**Hidden ticks and audio.** Hidden ticks accumulate audio intent, and the batch flushes with its visible sibling so horizon and coalescing policy see the whole batch. A hidden tick also stays out of visible-interval telemetry: one a few milliseconds before its sibling would read as a false burst.

**Pacing has two deadlines.** `NetplayPacingDeadline` keeps an ideal absolute deadline, which stops `Sleep` drift and holds long-term game speed, and an earliest-visible deadline at 7/8 of a frame, which stops a late frame being followed by a burst. Both are sampled at the completed-frame boundary, so native execution time is part of the interval. `observePresentationCompleted` can only extend the visible floor, to half a frame after the renderer finished, so a slow present does not get charged twice as slow motion. `anchorPresentationCompleted` anchors the first owned slot to the end of the DirectBoot rendezvous, so the ownership handoff cannot expose a short frame pair. `setFrameTicks` changes the period without touching the deadlines, because `configure` resets them, and a reset in the middle of a match shows as a hitch.

Each missed frame has one repayment path. Lateness within a frame is absorbed by the deadline. Beyond that, `poll` rebases and reports whole frames, and visible cadence never compresses to repay them. Eagerly moving the ideal deadline during holds would turn repeated 1 ms waits into permanent slow motion.

**Balance correction speeds up only.** A session can become lopsided with equal settings: one side near its prediction ceiling, the other predicting almost nothing. The low-depth side is receiving input early, so running its own clock slightly fast also sends its input earlier, which returns room to the other side. `planTrailingPaceCorrection` can only return zero or a negative period change, so online play never runs below the native 60.60 Hz. It changes how fast wall time is spent, never which frames run or with what input, so two peers slewing differently still compute identical frames. Any missing or stale sample means native cadence. A peer leading is never held for it either: the prediction ceiling already regulates that, from data the peer owns, while a peer-report comparison is one-way-transit old.

**Input production is fenced.** `produceLocalInputAhead` breaks a bilateral stall. Without it, one side stalls, stops producing input, the peer reaches its prediction cap and stalls too, and neither can leave that state below full speed. `inputProductionPermitted` stops it at exact barriers, during the rematch prompt, and outside rollback gameplay. At a barrier the peer waits on the barrier and not on our input, so more frames buy nothing. There is also a sharper reason: `captureLocalInput` adopts a word already stored for its target frame, so pre-filling frames that a lifecycle decision is about to write would replace a real press with the held word, and no error appears anywhere.

**Reading the run state in logs.** `classifyRunState` names what the session was doing, so 40 s at the prediction ceiling and 40 s suspended in the background are told apart. It is downstream of `planSchedule` and decides nothing. Precedence runs from session end, through external suspension and lifecycle barrier, to the network states. `PredictionPressure` means canonical time still moves but a peer is within the warning margin of its ceiling, and is also the label for our own late input, which is local and needs no peer action. `HoldNegotiating` is declared and never returned. A native clash or super flash stops no canonical frames, so it can never open a hold episode. `HoldEpisodeLedger` is fixed-size because a session may sit in hold indefinitely. `observeRecovery` credits only the first correction after an episode as its recovery depth, since mispredictions happen constantly and crediting all of them would over-count.

**Telling loss from a stalled peer.** Every `Inputs` packet carries a `PressureReport`. If `inputProducedThrough` advances but our contiguous prefix does not, packets are being lost. If `inputProducedThrough` does not advance, the peer stopped producing. The two look identical from the prefix alone, and they want opposite fixes. The report is advisory: no canonical path reads it, since it is one-way-transit old and a peer can send only defaults, and a receiver may use it only to steer its own wall-clock deadline. A report at an equal frontier is accepted if its datagram sequence is newer, because a peer stuck at its ceiling keeps the frontier fixed while its run state changes.

**Which tick draws.** Visibility must be computed against the last tick that actually runs. `playerCatchupTickVisibility` takes that as `lastIndex`, so a caller that truncates a batch has to shrink it. Measured against the planned count, a batch ending early draws nothing, and the player watches a still image of a match that is still running. Native FPS accounting is presentation-only, so hidden ticks restore it with `restoreQohNativeFpsAccounting`.

Spectator catch-up separates two questions. Visibility decides whether a tick owns the cadence slot, counts toward presentation rate, and makes sound. Drawing decides whether its pixels reach the screen. Tying them together gives one frame per slot, which is 1x playback and never converges on a backlog. The drawn frame must also close the batch: a present costs more than the whole slice, so drawing every tick, or the first, truncates the slice after one tick, while hidden ticks are cheap enough to fit first. The forced closing tick in `spectatorCatchupExecutedTickVisibility` is also the batch's audio boundary. Classifying it as historical accumulates unflushed sound intent until the bounded journal fails closed and effects go silent.

### Confirmed-hash desync detection

A frame is **confirmed** when it has been simulated and every remote input through it is known and matched. `updateStableConfirmed` will not advance while a mismatch or replay is pending, so a confirmed frame's `preStateHash` always describes the final timeline. It pushes `ConfirmedFrame` records in order.

`netPublishAndVerifyConfirmed` runs after every commit and on most hold paths:

1. **Pop confirmed frames.** Each must match `g_netMeta` on frame and hash, or the session ends. This is also where the recorder and spectator publication receive frames, so replays and spectators only ever see confirmed data. A spectator failure degrades the spectator service and never touches the player session.
2. **Submit.** Every 30th confirmed frame with a non-zero `preHash` goes out as a `NetConfirmedHash` through `netSubmitConfirmedHash`. Frontend frames have no `preHash` and are not compared here.
3. **Collect.** `netTakeConfirmedHash` feeds `g_netPendingPeerHashes`. A stale epoch is dropped, inconsistent epochs are terminal, and the queue is limited to 128.
4. **Compare.** A peer record waits until local confirmation reaches its frame, then must match on layout, epoch, `preHash`, all three RNG words, and both life values.

A mismatch writes the local components to the log via `netWriteConfirmedDesync` and ends the session with `kNetConfirmedDesync`. There is no resync. A divergence means the determinism model is wrong, and continuing would produce a match neither player actually played. The peer receives the terminal message, and `netCheckLink` logs its own components for the same frame, so both logs describe the failing frame.

The hash is of pre-state, so a desync report at frame N means divergence occurred at or before the tick of frame N-1. The check runs every 30 frames, so the first differing tick can be up to 30 frames earlier.

**Diagnostic files.** `netWriteConfirmedDesync` writes `confirmed_desync.jsonl` to the session folder, `netWriteLayoutDump` writes `layout_mismatch.txt`, and `netWriteConfirmedCheckpoint` writes a checkpoint for each submitted hash.

**`layout_mismatch.txt` is built to be exchanged.** The wire carries only five folded components, enough to say what differs but not whether it matters, so each machine writes this file the moment a mismatch is found. A peer timeout at the barrier writes one too, since this side announced and never saw an agreeing partner, and without it only the detecting side would have anything to compare. It contains no paths, bindings or names, because it may be pasted to a stranger. Regions of up to `kRawDumpLimit` bytes print raw. Wider regions print a fixed-count row of 32-bit chunk hashes, so diffing two files names an offset window, and the largest AI VM region costs the same line length as a small one.

**Sound files are not evidence of divergence.** A missing or replaced `.wav` changes nothing in the simulation, since status lengths come from `qgame::canonicalStatusSoundFrames` through `virtualSoundDuration`. When the layout digest's diagnostics show a mixer difference, the sound deadlines beside it name which status sound differs. The on-screen "compare your folders" advice keys off the word "differ" in the reason text, so a reason should use that word only when a file difference could cause it.

**The digest and what it may use.** `stateProofDigest64` is the bulk hash behind every state proof: the value peers compare and the value each `.q99r` frame records. It runs four independent 32-bit lanes, because a byte-serial 64-bit multiply is expensive on this i386 build and each frame takes two full passes. Two machines and every recording compare its output, so it may use only unsigned integer arithmetic and `memcpy` for unaligned loads. Floating point, CPU feature detection, intrinsics and anything derived from an address are all forbidden.

**Attribution.** The layout digest's components travel beside it in the fingerprint message, so a mismatch names which part differs. Sound and mixer state are outside the digest and so are never reported as the cause. Only a file-derived category should tell a player to compare installations. When every component agrees, the difference is inside a hashed region: `netLogLayoutDigest` logs those individually, and that is a developer's problem rather than a player's.

**Pointer children that alias.** A pointer child can resolve to memory another region already covers. Selecting one character copies a fighter struct into a helper clone verbatim, frame-data pointer included, so two owners name one heap. The duplicate is registered once and kept in `_ptrAliases`. `snapshotTo` re-checks on every save that the two still share an address and fails closed if not, so "these alias" is verified rather than assumed.

### Fail-closed termination

`netFailClosed(code, detail, why, notifyPeer, graceful)` is the only way a session ends. It is idempotent: the first caller's reason stands. The codes are `qnetplay::wire::TerminalErrorCode` values, so both sides agree on their meaning.

It does the following:

1. Sets `g_netHalted`, records the code and reason.
2. Shows `netShowTerminalStatus`.
3. Closes the replay recording.
4. For a non-graceful end, writes `netplay_failure.txt` to the session folder with session diagnostics, stall and hold breakdowns, transport counters and the current game thread stage (`netGetTransportDiagnostics`). A graceful end writes no failure file. The `netplay_end.txt` branch needs a reason beginning with "rematch", and no caller sends the terminate values that produce one.
5. If `notifyPeer`, sends the error with `netSetLinkError`. Failures that came from the peer pass `false`.
6. Clears `g_netGateOwnsTick` and the poll flags, and turns off the synthetic clock.
7. Seals any spectator archive.

From then on, `doNetplay` goes straight to `netPostTerminalHold`, which holds for `kNetTerminalGraceMs` (4 s) so the banner is readable, then requests a clean window close. The native tick does not run again, so no state is produced that could be mistaken for valid.

The design choice throughout is to refuse. Any unexpected session response, missing slot, mismatched tag, or impossible transition stops the match instead of continuing on a guess. Almost every call into `RollbackSession` and `RollbackManager` in the driver is checked this way. `kNetInternalFailure` is the common code. `kNetProtocolViolation` is reserved for bad peer data. `kNetTimeout` and `kNetTransportFailure` cover a lost peer. `kNetPeerRequested` with `graceful = true` is an ordinary ending, not an error.

**Peer liveness is judged after draining.** `doNetplay` applies a peer terminal first through `netCheckLink`, since nothing queued outranks it, then drains accepted input, and only then calls `netPeerAlive`. The network and game threads are separate, so after a long game-thread gap a timestamp read before draining can predate traffic the pump has already accepted, and a peer that is still talking gets timed out.

**The terminal hold.** `netPostTerminalHold` slows to about 60 Hz with a heartbeat log line every 10 s. It keeps wrapper pacing ownership: the wrapper keeps repainting the last published snapshot, and releasing ownership would expose a possibly mid-replay surface and re-enable the online debug menu. The close uses `WM_CLOSE`, so the game's own shutdown saves its config and exits, and the caster sees the exit and returns to its menu. The close latches only once the message has actually been posted, so an unresolvable window retries and does not hang. `[netplay] TERMINAL` is a fault marker the harness scans for, and a graceful end must not log it.

**How a session's end reaches the other machine.** `netSendGoodbyeNow` sets the local terminal and flushes it once synchronously, so a peer learns of a clean quit at once and does not wait out its 10 s liveness timeout. The pump keeps resending until the socket closes. A held terminal ends through the game's own window-close path, which saves `QOHcnf.key` and exits normally. The game can own two top-level windows, and posting to only the first can close the auxiliary one and leave the real loop alive, so every visible window with the game's title token gets the message, excluding IME helper windows.

**Transport rules that protect this.** On Windows an ICMP port-unreachable from an earlier send appears as `WSAECONNRESET` on a later `recvfrom`. It is not socket corruption, and peer liveness owns the disconnect decision, so it counts as an empty turn. Event mode in the pump is an optimization: any failure to wait or enumerate calls `disableEventMode` for that pump generation, and the loop uses the bounded nonblocking drain instead of spinning, which matters under Wine.

**Ping before the peer is alive.** Route latency publishes nothing until the peer's first accepted input sets `g_routeWindowStarted` and resets the estimator. The first probe goes out before the peer's pump exists, and its reply measures the peer's process boot. Dwell correction cannot catch that, because the datagram sat in a socket buffer and the peer honestly reports no dwell, and stall rejection cannot either, because our own loop was fine. `setLocalStallRejectMicros` handles the opposite case: a pump iteration past 25 ms means this thread was descheduled, so a late reply is our lateness and not the wire's.

### DirectBoot and the native selector

These are native facts that `src/game/DirectBoot.cpp` relies on. `kMatchFormationRow` holds the applied VS formation, not a game mode: 0 is none (the Arcade and Story state), 1 is two fighters (VS-list rows 0 and 1), 2 is four fighters. `kVsFormationApplied` means a formation has been applied. The native VS-list confirm has already set both to 1 for two-fighter rows, so the boot observes them and never overwrites them. Writing 0 selects Arcade behaviour: one selector at its ready state is enough to launch, so P2's character gets chosen for them, and the round-end handler vacates the loser's slot so the loser plays as CPU until pressing a button.

The CPU slot mask moves. During character select it reads 0 for every row, because only the native launch commit installs a row's CPU slots. Match init then copies it to `kMatchCommittedCpuMask`, distributes the bits to per-fighter flags and zeroes the original before the first match frame can present. Those per-fighter flags are in turn cleared by the round-result step, so `kMatchCommittedCpuMask` is the only surviving record of who is a CPU, and it is the engine's own test for crediting VS records. `rematchTheaterHumanVersusHuman` uses it for the same reason: a zero live mask looks like a CPU check and is true in every mode.

The launch commit is also the only writer of the final roster identity and palette bytes, and it sets the next scene itself. Forcing the scene transition leaves sentinel roster bytes, so replay playback must fire the commit at the native latch value of 30 and leave the formation pair as the VS-list confirm set it. In character select the native confirmation states must both reach 4, and writing the menu confirm flag there means "go back to the VS list". Netplay and managed offline stop at this screen and hand it to the players. Any extra frontend write in the path shared with netplay shifts `netBootstrapFingerprint`.

Ownership of the boot tick is granted one frame at a time through `g_offlineDispatchTicked`. The hooked scene dispatcher is preferred because it runs before the native one, and transient menu state may not survive until the render callback. Scene-1 and scene-3 detours exist only in some run modes, however, so the render callback has to remain the fallback for every other frame. Netplay and spectators tick only from the deterministic frame owner, so render cadence cannot mutate simulation state.

Training routes both selectors through P1's input source, which meets two native behaviours. The native handoff activates P2 inside the same selector loop that P1 confirmed in, so one press can feed both selectors; P2 is held inactive for two complete select steps and until every fight button is neutral. The stage screen's first branch accepts a held A as its confirmation, so two dispatches with all action buttons released are required at that boundary. Returning from Training gameplay also bypasses the initializer for the manual stage cursor, so the previous match's cursor would survive unless it is reset explicitly.

### The native continue screen as the rematch prompt

`netCanonicalRematchTick` borrows the Arcade continue screen. `rematchTheaterFlipPlan` writes exactly four words, and each is there for a native reason.

| Write | Native behaviour it answers |
|---|---|
| Formation row to 0 | Selects the Arcade continue behaviour |
| Match substate to 2 | Hosts the prompt. With the formation dropped, substate 5 restarts the round in place on its first tick, without input, so it must never dispatch while flipped |
| Next scene to 2 | The doorway already carries a pending scene change, and a granted tick must not leave for character select |
| Loser's bit in the vacancy mask | The prompt draws only when the loser's slot is vacated, and VS skips that write. Without it the prompt never draws and the native auto-advance arms. With it the timer branch holds, so the parked state is stable with no input |

The native confirm can fire only from a fresh button-ring entry, so `rematchTheaterLocalFeed` passes directions and no buttons. `rematchTheaterFlippedStateSane` runs around each step as the leak guard, and catches a substate change one turn before it could dispatch. Eligibility requires the doorway's first frame with the prompt window already matured (`rematchTheaterDoorwayEligible`).

### Delay and rollback changes

Input delay and rollback window can be changed during a match. Both are **peer-local**. Delay decides which frame a captured input lands on, and rollback decides how far this machine may speculate. Neither value is sent during play: `netRequestGameplayDelay` is reachable only from local controls and the test script, and nothing fed by a network message may set it. `netSetLocalGameplayDelayStatus` only publishes requested, applied and pending values to local statistics.

| | Delay | Rollback |
|---|---|---|
| Range | `kMinimumGameplayDelay` to `kMaximumGameplayDelay` (0 to 15) | `kMinimumGameplayRollback` to `kMaximumGameplayRollback` (2 to 30) |
| Step | `adjustedGameplayDelay` | `adjustedGameplayRollback` |
| Control | `InputDelayDecrease`, `InputDelayIncrease` | `RollbackWindowDecrease`, `RollbackWindowIncrease` |
| Allowed when | `runtimeDelayControlsAllowed`: rollback gameplay or scene 3, focused window | Same |
| Applied by | `tryChangeInputDelayAtConfirmedBoundary` | `tryChangeMaxRollback` |
| Needs | Fully confirmed frontier | Speculation not above the new value |

**Rollback** applies at once unless it is being lowered below live speculation. In that case `tryChangeMaxRollback` returns `Deferred`, and `netServiceDeferredRollbackRequest` retries from `g_netRollbackRequestPending` each opportunity. Raising is never blocked.

**Delay** is harder, because it changes the mapping from source frame to input frame. `inputDelayChangeBlocker` requires no replay, no pending mismatch, no capture for an earlier frame, and `_nextConfirmedFrame == _frontier`.

- During gameplay, `netRequestGameplayDelay` sets `g_netDelay` and starts `g_netDelayDrain`. `netApplyPhaseInputDelay` tries the change each frame. While the blocker is `UnconfirmedBoundary`, `doNetplay` holds at that frame so the peer's inputs can catch up.
- After `kRuntimeDelayDrainTimeoutMs` the request is cancelled and `g_netDelay` returns to `g_netGameplayDelayApplied`. A failed change does not end the session.
- Raising delay fills the new frames with neutral input through `storeLocal`, skipping any already assigned, and queues them for sending. The stream the peer sees therefore stays contiguous.
- Lowering delay leaves already-assigned future frames in place. `needsLocalCapture` is false until the frontier catches up, and those frames consume what was already sent.

**Outside gameplay**, `netApplyPhaseInputDelay` uses `netRefreshFrontendDelay` instead of `g_netDelay`. It is an automatic value from measured RTT with its own latch (`FrontendDelayLatch`, rising at most every 150 ms and falling every 300 ms), and it settles no lower than 1 or the player's gameplay delay. The player's gameplay delay is kept and restored on entering gameplay.

**The route floor** stops a player lowering a setting into a state that cannot hold full speed. The budget model in `src/net/PacingBudget.h` is about coverage in each direction: an input from A to B is covered by A's delay plus B's rollback.

```
hostReceive   = clientDelay + hostMaxRollback
clientReceive = hostDelay   + clientMaxRollback
```

`oneWayFrames` is half the RTT in frames, rounded up. The setup target and the two floors use it deliberately differently.

| Floor | Formula | Used by |
|---|---|---|
| Setup target | `requiredCoverageForRoute`: one-way plus `safetyMarginFramesForOneWay` (at least 2), from p95 RTT, plus 1 in `recommend` to reach `Classification::FullSpeed`. Advisory: it sets the recommendation and the coverage warning, and nothing enforces it | `computeRouteRecommendation` and the settings screen in `src/client/Main.cpp` |
| Setup floor | `floorFrames`: one-way alone, from mean RTT, taking the worse of the two sides' values. `enforcedOwnRollback` and `enforcedOwnDelay` add 1 to reach `Classification::FullSpeed` | `src/client/Main.cpp`, before committing session settings |
| Live | `netRouteLiveFloorFrames`: one-way alone, from mean RTT | `netMinimumLocalDelayForRoute`, `netMinimumLocalRollbackForRoute` |

The live floor is what physics requires, without a margin, so a player can trade smoothness for latency in a match but cannot request a setting the route cannot serve. It applies only to decreases, and refusals are logged with the numbers involved. Without an RTT measurement there is no floor beyond the fixed minimums. The peer's half of each sum comes from `g_netRemoteDelay` and `g_netRemoteRollback`, which are set when the session is armed and do not track the peer's later changes.

`recommend` produces the two presets offered on the setup screen, `Preset::LowLatency` and `Preset::Smoother`. `kCapabilityDelay` and `kCapabilityRollback` mirror the runtime limits.

**Which control helps whom.** From the coverage equations, this side's rollback is the only control that protects this side, and it needs no notice to the peer. Raising this side's delay improves the opponent's coverage and costs this side input lag, so a stalling player should raise rollback. That asymmetry is why the two floors cross over: the floor under this side's delay comes from the peer's rollback, and the floor under this side's rollback comes from the peer's delay.

**Frontend delay inputs.** `netRefreshFrontendDelay` steps toward its target one frame at a time in both directions. Rising only slowly is safe, since one frame short costs a brief exact-input wait and never a desync, while ten frames long taxes every menu press. It holds the baseline until the window has `kFrontendDelayMinSamples` samples, because the first samples come from both processes booting. The estimator also rejects a reply its own service loop read late (`setLocalStallRejectMicros`, 25 ms), so a local pump stall does not read as network latency.

**Frontend delay inputs.** `frontendDelayForRttMicros` adds one safety frame to the one-way frames, and two once the one-way tail reaches six frames, because one frame of phase slack no longer covers the exact-input turnaround at that range. It takes microseconds from the dwell-corrected estimator, so a sub-millisecond link resolves the same under jitter. `FrontendDelayLatch` moves one frame per interval in both directions, so one bad measurement cannot jump to the ceiling. While `windowUsable` is false it sits at the session baseline, since the window then holds only boot-time samples. `step` takes its intervals in the same unit as `now` and never assumes which, so the caller does the tick conversion.

### Recording and playback seams in the driver

**Only the confirmed timeline is recorded.** `netPublishAndVerifyConfirmed` is the one netplay source, shared with the spectator archive. Publishing at simulation time would save predictions. The match segment boundaries come from each frame's recorded phase tag, never live state, because by confirmation the simulation has moved on. The first segment opens at the timeline head, in the frontend, because it is the only one this file can boot on its own. Later segments set `independentBootstrap` to false and leave the RNG triple zero. `winningSideMask` and the round tallies are written as zero, since the confirmed stream retains neither and live values would belong to a later frame.

**Offline recording cannot copy netplay's seam.** `netOfflineRecorderCommitV2` borrows the game's completed-step counter as its clock, and that counter does not always move:

- **The setup tick does not step.** The launch transition is sampled on every call, ahead of the "has a frame advanced?" guard. Behind that guard it could never be seen.
- **Match-over does not step.** The counter stays still across the KO, win screen and continue doorway. A game closes only through published records whose before and after states are that `matchSub` 1 to 2 edge, and a proof cannot be attached to a later frame. So a record is due when the simulation stepped or the lifecycle moved, with a cap of 240 unstepped records against a lifecycle that oscillates.
- **Capture and submit are separate instants.** The capture is read at the setup instant, where seeds, FP words and roster are frame zero's. It is submitted only once the last published record ends in setup, because `GameEpochTracker` arms from the next frame's `before`, tolerates a scene 3 tail, and fails closed for the rest of the session on anything else. A rematch's previous state would be its win screen.
- **The epoch cannot be zero.** `buildFreshGameStartV2` refuses a zero `layoutGeneration`, so offline uses the constant `kOfflineLayoutGeneration` on the manifest and every lifecycle tag.

To diagnose, the `[replay.record-v2] offline lifecycle` line records up to 96 state changes, and the `offline fresh-game staged=` line records the coordinator state, since a refusal alone cannot separate an invalid capture from a previous game still open.

**Rematch writes land after the tick.** Written before the tick, the `matchSub` -1 would be consumed inside it and no pre-tick observer would see the launch. `netCanonicalRematchPostTick` writes it from inside `netRunOwnedTick` on every role, so the writes are canonical, inside the frame's post-state, and the frame's record has the same shape as a native launch commit. Epoch rotation and replay staging then run unchanged.

**The F1 return reaches viewers as set events.** Session events are not in the spectator input stream, and this scene change is the one no record carries. `spectatorSetEventPublishF1Return` chains the result and the return at the committed boundary, so a viewer performs the same dispatcher return at the same record. It must do so before consuming that record, since its inputs belong to the character-select dispatcher. If publication is impossible the feed seals, because a viewer must never consume character-select records while still simulating scene 2.

**Viewer files.** A viewer opens its own recording through the same launch function as the driver. An ordinary join starts at record 0, so it has the launch proof. A set-seek join does not see that barrier, so its joined game is not recorded, and the next game is. A file opened from a restored mid-game state could not be booted by anything. Set-seek also must not arm the game-start transient reset, because the restored bytes are already canonical and that reset would zero rings the archived hash covers. It presents and holds without calling `orig`, since one native tick on non-canonical input would fail the first hash comparison.

**Playback resolves frame zero against the recording, and only against an exact hash.** A real character select and the replay's direct boot can differ in small, known native state: the selector routes and launch counter, and the knockout latches. Playback searches those few candidate values until the full gameplay, FP and audio hash matches `frameZeroPreStateHash`. Nothing is weakened, because only an exact match is kept. The bootstrap fingerprint is a proof about the shape of the boot, not the state. When it differs but the phase and frame-zero hash match, playback first tries older `AudioManifestRule` values, then accepts and logs the difference. Per-frame proofs stay binding, and a real divergence cannot survive the first frame. This leniency exists only in playback, since a live session has no older recording to honour. The expensive field search, `netExplainBootstrapFingerprintMismatch`, runs only on a real refusal.

Setting `QOH99_REPLAY_PROOF_OBSERVE` to `diagnose` substitutes the recorded proof so a run continues past a frame-zero difference while every later proof is still compared and logged, which measures whether the difference matters. Normal runs never enable it.

### Rollback balance

Two machines with independent crystals drift apart, and catch-up cannot correct that: it repays transient debt, not a persistent phase error. Left alone, the faster side builds a lead until it hits its prediction ceiling on every frame, and it feels frozen on a clean link while the slower side reports no pressure.

`netRegulatePaceSlew` compares prediction depth, the real exposure, on both sides. The lower-depth peer shortens its frame period through `planTrailingPaceCorrection`, up to `kNetPaceCorrectionMaxPpm`, and returns to native cadence when the split closes. Nobody is paced below 60.60 Hz, because slowing the leading side is perceptible even with healthy rollback. Determinism is untouched: this changes how fast wall time reaches a frame, never which frame or input is used.

Three choices in it are load-bearing.

- **The signal is smoothed.** Remote input arrives in bursts, so instantaneous depth swings across the whole window within a second, and a controller reading it engages on noise and nets no movement. `netSmoothedPredictionDepth` uses the median of `g_netPredictionDepth`.
- **The peer's depth is the same kind of number.** The published depth is smoothed too, so both sides compare like with like. The peer's report must be fresh (`kRollbackBalancePressureFreshMs`) and the peer's state paceable (`rollbackBalancePeerStatePaceable`). Otherwise `classifyRollbackBalance` returns the reason and the slew releases to zero.
- **The deadband comes from headroom.** The lead the route allows is coverage minus transit, which is only a few frames across the recommended settings, so a fixed deadband could sit above anything reachable. `netPaceSlewDeadbandFrames` uses half the headroom, clamped to 1 through 3.

`QOH99_NP_DISABLE_ROLLBACK_BALANCE=1` restores native cadence for field diagnosis. `[rollback-balance]` logs each change of state and action.

A depth signal must not be replaced by local input history: during a stall the producer queues future input there, so it measures production lead, not simulation exposure. That production is the other half of staying balanced. A stalled simulation that stopped producing would starve the peer into its own ceiling and turn one machine's hiccup into a bilateral slowdown. `netProducerMaxLeadFrames` bounds the lead at the peer's rollback plus delay plus a small reserve, since past that the peer is blocked by its own ceiling. `QOH99_NP_DISABLE_INPUT_PRODUCER=1` turns this off in the same binary as a control arm, and `[input-production]` prints which arm ran.

### Reading the pacing logs

Once per reporting window the driver writes one set of lines that answer "why this frame rate".

| Line | Read it for |
|---|---|
| `[cadence]` | `presented` first. `ran` counts canonical frames, so a healthy `ran` with `presented=0` is a frozen screen over a running match. `want` is what 60.6 Hz would give. The wall clock is split into `runUs`, `exactInput`, `prediction`, `frameAdv`, `correction`, `setup`, each with a permille share, and the largest non-run bucket is the answer. `unaccounted` exposes a missing bucket. `koStage` and `exactWindow` tie stretched time to a stage of the native outro |
| `[presentation-starved]` | Frames advanced and nothing was drawn in the window. This is a scheduler bug, not the network |
| `[run-state]` | Residency per state. It separates time at the prediction ceiling (network) from time at a lifecycle barrier (routine), which produce the same cadence numbers |
| `[hold-episode]` | Count and worst duration of emergency holds, with the first missing frame |
| `[coverage-measured]` | Measured prediction depth beside the RTT estimate. Depth is how late remote input arrived, in frames, so its upper percentile is the coverage the route really demanded. `saturated=1` means the value is clipped at the window and the true requirement is higher |
| `[work-budget]` | Save, load and hidden-tick cost on this machine. `maxDepthAtP95` is how many replay frames fit in one 16.5 ms interval |
| `[input-production]` | `maxSpan` is the longest run of opportunities that produced no new input. Each one moves the peer closer to its own ceiling |
| `[peer-pressure]` | `inFlight` is frames the peer says it produced that our contiguous prefix has not reached. Persistently large with the peer advancing means loss on the path. Small while we starve means the peer really stopped producing |

**Spending time without a tick.** An outer slot has no duration of its own: the 60 Hz gate is charged per presented frame. A slot that skips its tick and only sleeps costs nothing, so several fit inside one visible frame's deadline and slow motion plays at full speed. `netPaceIdleSlot` consumes one frame period of the deadline, grants no tick and counts no presentation. It returns false when pacing is not owned or the clock has faulted, and the caller falls back to the cheap hold. Speeds above 1x have no such problem, since they are extra hidden ticks inside one paced slot.

### Native facts the driver depends on

These describe the game's own code.

- **Scene returns.** A zero from the match dispatcher is the ordinary move to character select. A zero from the menu or character-select dispatcher means the scene is finished, and the process exits.
- **The winner word.** `kMatchWinnerSlotAddr` means something only on the winner screen. It holds 0 for P1 and 1 for P2, and the dispatcher resets it to 0 on every round intro, so an earlier read credits P1 with every game. `netTrackCompletedGameWins` waits for `matchSub` 2, and a latch keeps the tally stable across replays of the same transition.
- **Outro stages.** `koStageName` names the native result sequencer's stages. Stage 7 is an input gate waiting for a released button edge, stage 8 runs the 60-tick countdown before the destructive round reset, and the secondary zoom timer serves earlier stages only.
- **A fatal hit is not low life.** Negative life starts the result sequencer without a death animation, and it then waits forever for the loser's completion flag. Forcing a KO has to go through the native damage and reaction path, as the test driver does.
- **Sound gates.** Character select plays a voice only if its load succeeded, and sounds 2, 4, 6 and 8 are both voice slots filled from character folders and completion gates. Playing an absent buffer is a native no-op. So `virtualSoundDuration` answers from `canonicalStatusSoundFrames`, the stock lengths frozen in code. They are not a flat value, because the authored lengths set the pacing of round announcements.
- **Failure text.** The game writes every failure reason to `SYSTEM\QOHerror.txt`, truncating per run and falling back to the Windows folder, then exits with code 1, which is also its normal exit code. `hkGameErrorWrite` mirrors each line to the log as `[game-error]` in UTF-8, which turns "the game closed" into a named cause.
- **Randomness and time.** Every RNG draw goes through one function, hooked as `hkRngDraw`. Per-frame effect emitters spawn a clock-dependent number of actors, each drawing gameplay RNG, so identical state and input diverge unless the clock is controlled. The "sound script" interpreter is the per-character AI engine, so its draws are real gameplay RNG. That is why `QOH99_NP_RNG_WHITELIST` is empty by default: redirecting those draws changes AI behaviour.

### The determinism self-test

`QOH99_NP_MODE=selftest` runs `doSelfTest`, which proves the region set complete without a peer: save S(N), tick, snapshot, restore S(N), tick again, compare. Any difference is state the savestate fails to capture. It advances one net frame per frame at about twice the CPU cost, with doubled sound.

Environment switches hunt a missing region. `QOH99_NP_WIDE_DESYNC=1` captures the game's whole data section and reports differing ranges outside the save set. `QOH99_NP_FEEDER_RING=1` keeps a per-frame ring of that image, for deep replays. `QOH99_NP_WIDE_RESTORE=1` (or a low-high range) restores non-saved data at replay entry, so bisecting the range finds the region. `QOH99_NP_FREEZE_CATCHUP=1` also removes the native catch-up sub-step from the first tick.

### Hiding native HUD elements

HUD profiles (`src/game/NativeHudPolicy.h`) can hide native HUD elements in Training and local replay playback, and none of it reaches a peer, a replay file or a hash. Producers that only draw are skipped outright. EX and the combo readout also advance state, so skipping them would change the match.

For those, `nativeHudBeginDescriptors` records the list head and the two per-frame bump counters, the original runs in full, and `nativeHudDiscardDescriptors` puts them back. Natively, descriptors are pushed on the head of one list, so restoring the head drops exactly what the synchronous call added. The head restore cannot depend on a counter having moved, because EX links pre-allocated nodes without touching one. `kNativeHudListHeadPtr` is a pointer variable, and the head lives 4 bytes into what it points at, not 4 bytes past the variable. Every detour is fail-open: a signature mismatch leaves that element visible and everything else working.

### Leaving a match offline

`netOfflineReturnToCharacterSelectTick` is F1 in offline versus. It arms the same dispatcher zero netplay takes after the session-event ladder, with no peer to agree with.

- **Scene 2 only**, since anywhere else a zero exits the process.
- **Versus human only** (`rematchTheaterHumanVersusHuman`). In VS CPU or Story it would drop a run into a versus character select.
- **Not during the rematch prompt.** The prompt holds the post-game words, including the next-scene word the return would consume, and nothing else serializes the two offline.
- **The recording closes first**, on the same terminal as the online return, and a refused close does not hold the player in the match. `g_offlineRecorderRebaseline` then re-seeds the offline commit seam. The return does not call the original, so the frame counter stands still, and the next publication would otherwise open a new match carrying the old match's lifecycle tag and pre-state hash.

The edge comes from the training block's evaluation in the same tick, because `g_inputOuterIteration` is advanced by netplay-only code and any other offline evaluation returns a cached empty result. `OnlineReturnCharacterSelect` and `TrainingReturnCharacterSelect` share F1 by default, which is legal only if they are never enabled together, so `offlineReturnEligible` removes the training action for as long as the offline action is on.

The offline prompt, `netOfflineRematchTick`, opens the native Arcade continue screen by writing post-game words, saved first in `savedDwords`, without changing scene. Two YES answers release `matchSub` to 5 under the flip, because the native Arcade branch restarts the match in place with the same roster. The VS formation is put back the moment that reinitialization arms. Any NO restores the saved words and the native versus exit lands on character select. Each tick checks `rematchTheaterFlippedStateSane`, and any deviation means an edge reached the Arcade machine, so the words are restored at once and logged as `LEAKED EDGE`.

### Session settings and the saved profile

A session overrides fields of the native profile, listed in `kQohSessionOverrideOffsets`, and the game saves the whole profile on exit. `hkConfigSave` therefore stages the player's originals with `netStageOriginalConfigForSave` around the native write and puts the runtime values back afterwards. Replay playback needs this as much as netplay: bootstrap forces the recording's match rules into the active profile, and without staging, watching a file would overwrite the player's settings. Forcing a value for the session is fine, but rewriting the player's `QOHcnf.key` is not. The music backend is not in the list because it is peer-local presentation.

The native code calls the config writer from one place, its shutdown path, so reaching `hkConfigSave` means the process is exiting. That makes it the place to act while workers are still alive. A clean local quit during a live match sends `netSendGoodbyeNow`, so the opponent ends at once and does not wait out liveness, and `g_netHalted` keeps a peer-initiated close from notifying back. `DllMain` cannot do this work, because it may not block under the loader lock.

### Explaining a refused bootstrap proof

`netExplainBootstrapFingerprintMismatch` runs once when a replay's bootstrap proof differs. The proof is a pure function of `kNetFrontendFields`, so it searches for the single field value, small values and then single-bit flips, that reproduces the recorded hash under each `AudioManifestRule`, and logs `[replay.explain]` with the field and both values. It only reports, and the refusal stands. Live sessions always use `AudioManifestRule::None`. The other rules stay computable because existing recordings folded a sound manifest into their stored proof and must keep playing.

## The client: `qoh99caster.exe`

The caster owns everything that happens before the game's main thread runs: it decides whether this install may play, finds the other player, measures the route, agrees delay and rollback, and starts the game with a socket that is already known to reach the peer. After that it mostly waits, and serves spectator requests if it is the host.

The code lives in `src/client/Main.cpp` (one large translation unit holding the verbs and every screen), `src/client/Launcher.cpp` (process creation and injection), `src/ipc/` (the contract shared with the DLL) and `src/net/` (sockets, links, route logic).

### Control flow at a glance

```
main()
  antivirus preflight (warns only), session log
  relay / replay / probe / analyze-*  -> leave here, parseArgs never runs
  menu -> interactive()               -> same run* functions as the flags
  host -> runAutomaticHost | runHost (--relay)
  join -> runAutomaticJoin -> runJoin

run*:  admission -> sockets -> control link -> hostNegotiate / negotiation in runJoin
       -> UDP route -> measure -> settings screen -> commit
       -> launchOnlineSynchronized
            prepareInjected   (game suspended, DLL in, hooks live: "Stage A")
            MSG_RUNTIME_READY both ways, then MSG_RUNTIME_START both ways
            resumePrepared    (main thread runs, input ready: "Stage B")
            waitForLaunchedGame
```

`main` returns for `--tests` and `help` before anything else runs. `antivirusPreflightShouldSkip` turns the antivirus check off for `relay`, `analyze-*`, `--help`, `--version` and the Wine fixture, and whenever `--game` or `--dll` is passed; `probe`, `replay` and `replays` still run it. `preflightAntivirus` only tests that the caster, the netplay DLL and `ddraw.dll` can be opened in the caster's folder, and notes whether the last run ended abruptly. It never blocks: `main` prints advice and carries on.

### Admission and preflight

Admission answers "is this exactly a build we hold addresses for?". The file hash decides, never a file date.

`validateRuntimeInstallationForAdmission` in `src/client/BuildIdentity.cpp` runs `detectKnownRuntimeExecutable`:

1. `inspectExecutable` reads PE machine, size and SHA-256. A non-i386 file is refused outright.
2. `canonicalizeKnownExecutableVariant` maps an attested physical hash to its canonical one through `kAttestedVariants`. Those two November files differ only by a 26-byte gate the wrapper removes at startup, so the running images match. `ExecutableIdentity` keeps both `physicalSha256` and `sha256`; only the canonical hash reaches the compatibility identity.
3. `findProfileBySha256` and `findProfileByExactExecutableEvidence` must both hit a registered runtime profile.

`inspectRuntimeInstallationWithVerification` then has `SimulationContentManifestBuilder` hash the simulation content under the game folder, and `makeCompatibilityIdentity` folds everything into a `CompatibilityIdentity`: profile, executable digest and size, content roots, state schema, simulation and timing ids, wire version. `admissionAllowed` on the manifest gives the verdict.

Admission is consulted at three points per online session, for different reasons. The first two share one cached result, so the files are hashed at most twice per path:

| Where | Gate | Purpose |
|---|---|---|
| `gameFilesCheck` | Admission, cached per path in `g_gameFiles` | Fail at the menu, not after a handshake. Fills `g_preflightInstallation`, the source of the identity sent to the peer |
| `preflightOnlineArtifacts` | `gameFilesCheck` plus `preflightLaunchArtifacts`, the latter uncached | DLL contract and `ddraw.dll` beside the game, checked before any socket opens |
| `preflightInjectedLaunch`, inside `prepareInjected` | Fresh admission, never cached | Compared with `semanticallyEqual` against `cfg.compatibilityIdentity`, so files edited during a long lobby fail the launch instead of desyncing the match |

`validatePackagedDdrawWrapper` requires a PE32 i386 DLL that exports the pacing ABI. It parses the file and never loads it.

The caching in `gameFilesCheck` decides how often the UI repeats work, never whether the answer is trusted. Besides the fresh check in `prepareInjected`, the DLL identifies the running image for itself in `netVerifyHostImageOnce` and refuses to continue if that fails. `gameFilesCheck` writes process-wide state that the menu reads, so a worker thread must not be the first to call it for a new path; the replay browser settles it on its own thread before starting its background identity work.

The replay content root comes from the same place as the netplay identity: `replayContentRootFor` returns `simulationRootSha256`, not a hash of the whole game folder. A root covering every file would invalidate all existing replays as soon as an unrelated file, such as a voice clip, appeared in the folder, even though the simulation had not changed. `needReplayContentIdentity` changes no work and only records what each caller wants.

`portablePath` resolves package-owned relative paths beside the caster first and against the current directory only after that, because a launcher's "start in" folder is not guaranteed to be the install folder under Proton. Rooted paths are used as given.

### The build contract

Four independently built things must agree: caster, netplay DLL, wrapper, and the other player's copy. Each layer catches a different failure.

| Check | Enforced in | Catches |
|---|---|---|
| `QOH99_BUILD_CONTRACT_MARKER` | `validateNetplayDllContract` | A caster next to a DLL from another build |
| `kIpcMagic`, `kIpcVersion`, exact byte count | `readIpcConfig` in `src/DllMain.cpp` | A marker check that was bypassed, or a truncated pipe write |
| `qnet::kProtocolVersion` in `MSG_VERSION_CONFIG` | `hostNegotiate`, `runJoin` | Control-protocol mismatch between players |
| `CompatibilityIdentity` | `compatibilityMismatch`, on both sides | Different game build or content between players |
| `RuntimeArtifactIdentityWire` | `runtimeArtifactMismatch` | Different caster, DLL or wrapper bytes between players (`sha256ArtifactFile` hashes each) |
| `kRuntimeCompatibilityId` | `runJoin` on `NetplayConfig`, `readIpcConfig` | Simulation-affecting code change with otherwise matching wire formats |

The marker in `src/ipc/BuildContract.h` is a long string naming every versioned ABI. The DLL exports it as `qoh99NetplayBuildContract`, so it sits in the PE image as data, and the caster finds it with a byte search without executing the DLL. It works because both binaries compile the same header. The `static_assert`s beside it pin `kIpcVersion`, `sizeof(IpcConfig)`, `qnet::kProtocolVersion`, `qnetplay::wire::kVersion` and the spectator and replay versions, so changing any of them breaks the compile until someone edits the assertion, whose message asks for the string to change with it. Nothing compares the string with the numbers, so the new marker depends on following that message. Once the marker changes, a DLL built before that change fails the check.

**Telling the player whose problem it is.** `identityMismatchReason` is the engineering name and belongs in logs. `identityMismatchPlayerText` sits beside it in the same switch order and is what the other player reads, because the two causes look alike in engineering terms and have unrelated fixes: one means the netplay build differs, the other that the game files differ. A new `IdentityMismatch` value needs wording in both.

The per-artifact digests in `RuntimeArtifactIdentityWire` are compared only between live peers and are never a replay condition. A recording needs the same simulation. A live session also needs both launchers and both wrappers to behave in lockstep.

### Sockets

A host holds three UDP sockets with three owners. Keeping them apart explains most of the code that follows.

| Socket | Port | Owner | Carries |
|---|---|---|---|
| Control (`HostControlEndpoint::socket`) | `s.port`, the shared address | `UdpControlListener` | Control messages, autopunch for the control path |
| Match (`udp` in `runAutomaticHost`) | `udp.open(0)`, OS-assigned | Caster, then the game | Route probes, then all gameplay |
| Spectator (`spectatorUdp`) | See "Port mapping" | Handed to the sidecar | Spectator traffic only |

The joiner has its own OS-assigned `joinControl` and match sockets. The host publishes its match port as `InitialConfig::dataPort`, and the joiner pairs it with the IP it typed. Ordinary play opens no TCP listener and no TCP control channel. Outbound TCP is limited to UPnP, public-IP detection and an optional relay.

### The UDP control channel

`ControlLink` in `src/net/ControlLink.h` is the interface the negotiation code talks to. `UdpControlLink` is the normal implementation and `TcpControlLink` the relay one, so `hostNegotiate` does not care which it holds. Every datagram is a `packMsg` frame: magic, type, CRC of the body.

**Establishment.** `startClient` picks a nonce with `secureRandom64` and sends `MSG_LINK_HELLO` to every hello target each `kLinkHelloIntervalMs` (100 ms). There can be up to `kMaxHelloTargets` targets, because a NAT mapping learned while dialing is added with `addHelloTarget`. The host echoes the nonce in `MSG_LINK_HELLO_ACK`. The client accepts only a matching nonce from an address it `greeted`, then adopts that source as `_peer`. A punched peer usually answers from a NAT port nobody typed, so this adoption is what makes the rest work.

**Reliability.** Once established, `ReliableChannel` adds sequencing, resends every `kResendIntervalMs` (50 ms) and keepalives. The link closes after `kLinkIdleTimeoutMs` of silence, or when `kLinkMaxUnacked` messages are unacknowledged.

Invariants that matter when editing this file:

- **One reader per socket.** `UdpControlListener` owns the host socket and routes by source in `dispatch`. Its links get `setExternalPump`, so `recvMsgEx` pumps the listener instead of reading the socket, including on a zero timeout. A `timeoutMs == 0` path that skipped the pump would send but never hear. The `_pumping` flag stops re-entry, since links call back into the listener.
- **Size is refused early.** `sendMsg` rejects anything over `kLinkMaxDatagram` before `ReliableChannel::send`, which consumes a sequence number first and would block its window forever on an unsendable entry.
- **Links do not move.** The reliable channel's callbacks capture `this` and `&_peer`, so the class deletes copy and move.
- **Unknown datagrams are not errors.** The same socket receives autopunch traffic. `classify` returns false for them and they go to the foreign sink. `dialControlLink` detaches its sink on every exit, because the sink captures a stack-local `AutopunchSession`.

The link authenticates nothing beyond the nonce echo and carries no encryption. The match credential is the 128-bit cookie from `generateSessionCookie`, which travels inside `MSG_NETPLAY_CONFIG` over this channel. A listener can hold `kLinkMaxPeers` links. Non-join visitors get a `MSG_SPECTATE_INFO` answer and are closed.

**Why an ack needs both a nonce and a source.** The nonce says the answer belongs to this attempt and not to an abandoned one. The source check says it came from an address the client chose to greet. Without it, anyone who merely observes a hello could answer first from any address, and the first ack establishes the link that later carries the session cookies. This check is only on the client's reply path. The host deliberately does not compare a hello's source with an address learned elsewhere, since a punched or LAN peer often arrives from a different one.

**Failure shapes.** The hello carries the protocol version but the link does not enforce it, so a mismatch reaches `MSG_VERSION_CONFIG` and its specific "update the older side" message instead of a silent timeout. A listener with no free slot drops the hello without answering: the peer retries anyway, and a "busy" message would only tell it to do that. An established link answers a repeat hello only from the same peer and nonce, which covers a lost ack. Anyone else is a second connector and is refused.

**Why the idle limit is long.** `kLinkIdleTimeoutMs` is 90 s, far above `kKeepAliveTimeoutMs`, because a peer legitimately goes silent during the handshake while `prepareInjected` creates the game and waits for the DLL. A 10 s rule would kill the channel on a slow machine at the last step before the match, and it would look like the opponent leaving. `pumpRecv` waits are what detect an absent peer promptly; the idle limit is only the backstop for a link nobody is waiting on.

### Negotiation order

`pumpRecv` is the receive primitive. It waits for one expected type while answering `MSG_PING`, and stashing `MSG_SETTINGS_PENDING` and `MSG_CONFIRM_CONFIG` in `PumpCtx`, so an out-of-order confirmation is not lost. `MSG_ERROR_TEXT` always surfaces as `Pump::RemoteError`.

| # | Direction | Message | Check |
|---|---|---|---|
| 1 | H to C | `MSG_VERSION_CONFIG` | Version equality, reported through `MSG_ERROR_TEXT` |
| 2 | C to H | `MSG_VERSION_CONFIG`, `MSG_CLIENT_INTENT` | `validClientIntentRecord`; a non-join intent ends here |
| 3 | Both | `MSG_INITIAL_CONFIG` | Name, match port, identities |
| 4 | H to C | `MSG_PING` burst, `MSG_PING_STATS` | Diagnostic only; never feeds tuning |
| 5 | H to C | `MSG_NETPLAY_CONFIG` | Session id, both cookies; cookies must differ and be nonzero |
| 6 | Both | `MSG_ROUTE_CANDIDATES` | `acceptCandidateAdvertisement`, `validateCandidateSet` |
| 7 | UDP | Route race, rendezvous, sampling | See below |
| 8 | C to H | `MSG_TUNING_PROPOSAL` | Host must already hold that route validated |
| 9 | Both | `MSG_SETTINGS_PENDING` while on the screen | Live mirror every 250 ms (screen) or 500 ms (lobby) |
| 10 | C to H | `MSG_CONFIRM_CONFIG` | `finalConfirmationMatchesProposal`: route and limits may not change |
| 11 | H to C | `MSG_ROUTE_COMMIT` | Final host tuning and requirement |
| 12 | Both | `MSG_RUNTIME_READY`, then `MSG_RUNTIME_START` | `validRuntimeSync` |

Any host-side failure before launch returns `HostAttempt::LoopBack`, and the host goes back to waiting. A bad joiner does not end a hosting session, except under an explicit `host --relay`, which holds one relay pair and exits when that attempt fails.

### Route selection

There are two separate decisions. The control link must reach the host first; the match route is then chosen on a different socket. The client decides and the host only validates, which is why `hostUdpRendezvous` has no race of its own.

**Reaching the host.** `dialHostControl` calls `dialControlLink`, which sends hellos straight at the typed address while an `AutopunchSession` looks up the host's NAT mapping. The host's `HostControlEndpoint` stays registered with the punch service and uses `AutopunchPeerSet` to punch back at each joiner it hears about. The budget is 15 s with a relay fallback available, 20 s without.

**Candidates.** `buildCandidateAdvertisement` lists up to `kMaxPrivateCandidates` LAN addresses and reflects `observedPublicIpv4BE`, the address this side saw the peer use. The joiner adds the typed address when the host did not advertise it. Route ids are not arbitrary: 0 (`kCommittedRouteId`) is the committed-route sampler, 1 to N are advertised candidates, and 255 and 254 are local ids for the typed address and a punched mapping.

**The race.** `raceDirectCandidates` sends `RouteProbe` datagrams at all targets for up to 1500 ms. After a 300 ms settle, a candidate counts as validated at 8 unique replies, and `selectRoute` in `src/net/RouteSelection.h` picks the lowest p95. Within `tieEpsilonMicros` (3 ms) a LAN route wins, then lower loss. A public route whose address equals your own becomes `PublicDirectPossibleHairpin`.

**Autopunch fallback.** If nothing validates, `autopunchFallbackPass` runs for up to 4 s. A discovered mapping gets its own one-entry race, and the winner goes to `clientUdpRendezvous`. Without any winner, the typed address is tried there anyway.

The word "relay" is overloaded here. `kRelayHost` in `src/net/Autopunch.h` is the public rendezvous service `delthas.fr:14763` (override `QOH99_PUNCH_RELAY_OVERRIDE`). It sees 2-byte registers and 8-byte lookups, returns mappings, and carries no match traffic. A traffic relay is a different thing, described below. `handleInbound` accepts a mapping only from the service's own address and, when a target is set, only for the target IP, since a forged mapping would redirect punch bursts. `setTarget` takes the port in host byte order while candidates carry it in network order, so `autopunchFallbackPass` converts with `ntohs`. Passing the wrong one looks up the wrong port and simply never finds the peer.

**Rendezvous and locking.** `NetplayCore` runs the Hello exchange on the match socket. The client sets `expectedPeer` to the winner. The host uses `learnPeerPort`, and the first Hello carrying a valid cookie locks the peer: the cookie is the authenticator, so the source address may legitimately differ from the control link's. `driveUdpCore` fixes the receive order as punch, then probes, then core, so every datagram has one consumer.

**Traffic relay path.** This runs only if a relay exists: `configuredRelayServer` reads `QOH99_RELAY_OVERRIDE` and nothing else, or the user passes `--relay`. With neither, `automaticRelayOptions` fails and `runAutomaticJoin` goes direct-only, which still includes hole punching. No public traffic relay exists.

With a relay configured, `deriveEndpointRoomId` lets both players compute the same room from the host's advertised `ip:port`, so no room code is exchanged. It also means a joiner who typed a different address than the advertised one lands in a different room. `runAutomaticHost` keeps the relay room waiting alongside the direct listener and closes the room if direct wins. `runJoin` recurses into relay mode when the dial fails, when the control link dies before version exchange completes, or when direct UDP fails.

In the relay rendezvous, `RelayUdpClient` supplies a punched direct target and the relay endpoint. `RouteSelector` probes direct immediately and adds relay after `kDirectGraceMs` (1500 ms). Direct wins whenever it establishes, relay wins once established after the grace, and `kRouteDeadlineMs` (12 s) ends the attempt. `retainRelayLease` keeps the TCP control connection open until the game exits. `--force-relay` is a diagnostic override.

**Tie-breaking favours the typed address.** The typed endpoint enters the race first, and `selectRoute` keeps the earlier candidate on an effective tie unless a later one is a LAN route against a non-LAN best, or has lower loss. A candidate therefore displaces the address the player chose only for a real gain.

**Silence is the only dial failure.** A datagram dial has no equivalent of a refused connection, so the dial budget is the failure detector. Silence covers both "no host here" and "a host still running its preflight", which can take seconds on a loaded machine. That is why `kDialWithFallbackMs` and `kDialOnlyRouteMs` are long. Shortening them hurts more than it appears to, because falling back to a relay is one-way and direct is not reconsidered afterwards.

**A busy host closes after answering.** A host in a match answers a newcomer with `MSG_SPECTATE_INFO` and closes at once, so `pumpRecv` stashes the answer in `PumpCtx` and the joiner usually sees `Pump::PeerClosed`, not a timeout. Code that offers "watch instead" has to look at `haveSpectateInfo` on both endings.

**Host loop order.** `runAutomaticHost` polls the relay pair before it accepts a direct link. Once the relay reports `PairPoll::Ready`, that pair owns the session, so a late direct connection cannot strand a paired relay client for a whole direct game. While the room is only waiting, a queued direct link wins and the room is cancelled. If direct UDP then fails, the joiner retries the same derived room and the loop registers it again. Under `--force-relay`, `directListener` is false and established direct links are closed immediately, so they fail over quickly and do not fill the listener's slots. The control socket and its punch registration stay alive for spectators.

**Observed relay addresses.** `discoverObservedHostEndpoint` takes only the IP from what the traffic relay saw. The source port belongs to a short-lived TCP probe, so the host's configured UDP port stays authoritative.

**How a punch actually starts.** Neither peer can see the external port its NAT assigned. The rendezvous service can, and in observed behaviour it answers both parties when one looks the other up, so the lookup also tells the other side to start sending. A hole opens only if both sides send while the other's packets are arriving. A host that only registers is therefore waiting to be told, and it must keep registering, because a NAT mapping expires.

Three behaviours in `AutopunchSession` and `AutopunchPeerSet` follow from that:

- **The punch window starts when the peer's location is learned.** `handleInbound` resets `_punchWindowStartMs` on a changed mapping. Measuring from session start would leave a host that has been waiting for a minute unable to punch back, while its registration and mapping counters kept climbing and `punchesSent` stayed at 0. That combination in the log is the signature of this kind of fault.
- **A same-IP answer with a different port is accepted.** The service may return another registration on the requested public IPv4, because one caster registers twice from one address: the control socket and the spectator sidecar's socket. Rejecting it would stop the dialer ever sending a Hello to the socket bridging that port. `mappingSameIpAlternates` counts these. A different IP is still refused. `decodeMapping` also refuses any zero field, since a zero means the service has nothing, and acting on it would send punches at 0.0.0.0.
- **The spectator source asks too.** Because of that double registration, a viewer's lookup can notify the host's control socket and not the sidecar. A sidecar that only registered and waited would then never send outward, and the viewer's punches would die at a NAT with no reason to open. `noteAdmittedViewer` calls `addLookupTarget` with the viewer's port from `spectatorDataPort` in `ClientIntent`, because a lookup sent from the sidecar's own socket is answered to that socket.

`AutopunchPeerSet` holds state for many viewers, so a later mapping must not discard an earlier peer's window. `kMaxSourcePunchPeers` (32) and `kMaxSourcePunchPeersPerIpv4` (4) are transport limits, not admission: four endpoints leave room for a small household behind one NAT without letting one address take all the state. Viewer admission stays with QSP authentication.

**Unconnected UDP and ICMP.** Windows turns an ICMP "port unreachable" for an unconnected UDP socket into a queued `WSAECONNRESET`, so one closed punch target could leave a backlog of resets in front of a valid Hello from someone else. `disableUdpConnectionReset` turns this off with `SIO_UDP_CONNRESET`. Wine and unusual providers may ignore that ioctl, so the receive path also skips reset errors instead of returning on the first one.

**IPv4 only, at the protocol layer.** `Endpoint` is a `sockaddr_in`, and the candidate and relay wire formats carry a 32-bit address, so this is not just a parser limit. `resolveEndpoints` filters to `AF_INET` through `resolveFamilySupported`, and an IPv6-only host fails with an explicit message. Adding IPv6 means widening `Endpoint` and those wire encodings first. `Endpoint::parse` stays numeric because it must never block on a name lookup.

**Preflight exists so that a request known to be impossible fails in the caster,** with one specific reason and no game process or capacity slot left behind. A wrong key, an explicit reject, a full host, a stale session or a contract mismatch are definite because the host answered. Silence proves nothing: a source legitimately says nothing between printing its code and the players clearing READY, and an unreachable relay says nothing about a direct route. That is why `isDefinitivePreflightFailure` is true only for proved outcomes. `ok` always means a specific endpoint answered with a verified, session-scoped QSP message. A control exchange that fails transiently is retried up to `kControlExchangeAttempts` while `kControlExchangeRetryReserveMs` remains for the data race.

**The proved socket stays open across launch.** `runSpectator` takes it with `detachViewerSocket`, and with it the NAT mapping, punch state, local port and the source-side slot its Hello created. A new socket would say Hello as a different viewer, so with `maxViewers` at 1 a successful preflight would get the real viewer rejected as full. Three other things follow from one identity spanning both stages:

- The viewer id comes from `secureRandom64`, because it scopes Detach, Migrate and generation changes, and a derivable value would let an off-path attacker target a live viewer.
- The game must continue at `spectatorSequenceBase`. Restarting at 1 turns its Hello and every InitAck into replays. The source drops them until the sequences are used up (one per 250 ms resend), passes `handshakeDeadlineMs` (10 s), retires the viewer and spends the grant, so a late but successful preflight ends in `GrantInvalid` seconds after the window opens.
- The socket is bound before admission, so the intent can name its port. The source needs that port to look this exact registration up on the punch service. A caster registers twice from one public IPv4, and waiting to be notified can deliver the notification to the wrong socket.

**The order of the race matters.** The first admission is always non-relay, even when the address contains a relay room. Probing keeps running during the `kAutopunchAssistMs` (10 s) punch assist, a retry race follows it, and `kRelayLaneReserveMs` (or a third of a short budget) stays held for the relay so a long assist cannot starve it. Late answers are common because the source registers every two seconds and its punch burst runs for ten. Before asking for a relay grant, the automatic path sends a best-effort Detach and waits for the authenticated release from `releaseControlAdmission`. Reusing the direct grant creates a route-confused slot, and racing relay before the release creates two owners of one capacity slot. Detach alone proves nothing, since its cursor and digest may not describe a provisional viewer on the source.

**Trust rules on this path.** A punch mapping is accepted only from the service's exact address, and its fields must match an outstanding `PendingPunchLookup` (target internal port and observed address). This viewer's own port plays no part, because anyone can send eight bytes and a forged mapping would add an attacker's candidate to the race. Lookup targets are kept separately from probes because an endpoint first labelled `Lan` may still be registered on the service. The route layer performs no DNS: `Main.cpp` resolves the service and applies `QOH99_PUNCH_RELAY_OVERRIDE`, and an unresolved address only skips the assist. The relay retry loop waits out the whole `retryAfterMs` in short `waitMs` slices. Treating one slice as the whole backoff would reconnect tens of times a second, exhaust the relay's per-IP accept bucket and can starve a source registering from the same NAT address.

**Forced routes must prove that route.** A zero `directRaceMs` means no direct Hello at all, since one packet on localhost can be answered in the same pump turn and turn a forced-relay test into a direct success. A forced punch clears the probes before pumping the mapping for the same reason.

**Diagnosing a failed attempt.** Each probe logs `helloSent` and `punchEchoed`, which separate "we never spoke to this address" from "we spoke and it never answered", a wrong candidate from an unreachable one. `sawForeignSourceTraffic` separates "the route works but the credential was refused" from "nothing answered". If `stage=assist-mapping` names an `internalPort` that was not asked about, the service answered about a different socket behind the same address. A fresh viewer refused at record 0 holds no prefix to disagree about, so the cause is that bounded retention already dropped the start of the set, not that the session ended.

### Measuring the route

`RouteProbeService` measures the same socket the match will use, on the route already chosen. `sampleCommittedRoute` probes every 12 ms until 32 unique replies or 2 s, whichever comes first. A short window is allowed; it shows as "so far" on the screen and in `windowComplete`.

`RouteRttEstimator` subtracts the peer's `dwellMicros`, so the RTT excludes time the answer sat in a busy loop. `consume` drops anything failing `decodeDatagram` against the session id and cookie, answers probes through a 16-token bucket, and accepts an ack only from the `dest` of that route id. Answering must continue while a player sits on a screen, which is why `recommendationScreen` and the confirmation loops call `svcPumpUdp`; an unanswered probe would read as loss on the other side. `exportPrelaunch` copies the result into `IpcConfig` as the `prelaunchRoute*` fields.

**The window closes, and that matters.** Both sides establish within milliseconds of each other and run the same bounded burst in `sampleCommittedRoute`, so the two measurements overlap while both are pumping at burst cadence. `freezeTarget` then stops probing and leaves answering on. If probing continued, it would measure the peer's roughly 50 ms menu tick while the peer sat on the settings screen. `RouteRttEstimator` keeps a rolling window of 128 samples, so those late samples would replace the good evidence, not merely dilute it. Do not reopen a frozen target. For the same reason, control waits on the joiner during measurement use a 1 ms slice, since a longer wait would quantize its probe answers.

The responder's token bucket refills one ack per 250 us, well above any real sampler, with a 16-token burst that covers one probe sweep across all candidates. A flood is bounded and a racing peer's back-to-back probes are still all answered.

**Reading the logs.** `[route-stats]` with `window INCOMPLETE` means loss or a peer that stopped pumping, and the recommendation then rests on fewer samples. A `[settings-heartbeat]` timeout with `rx=` showing 0 means nothing was ever received on that link, so suspect the read path on this machine, not the peer.

**Which replies are not RTT samples.** `RouteRttEstimator` discards two kinds of reply. A retransmitted probe is `Ambiguous`, since it cannot be told which send the reply answers. A reply read after a long gap in the local service loop is `LocallyDelayed`: the receive stamp is taken when the loop reaches the datagram, and responder dwell cannot correct for this side being the slow one. A stalled pump could otherwise report hundreds of milliseconds on a 3 ms link, and that number would drive frontend delay. Neither counts as a loss, because the reply did arrive. The rule needs a caller that ticks: `src/game/DPlayTransport.cpp` calls `onServiceTick` and sets `setLocalStallRejectMicros(25000)`. Zero is the default and disables it.

`reset` bumps the generation so a reply from before a route change cannot repopulate the estimator. The stall threshold is configuration and survives the reset.

### The delay and rollback screen

There is no tuning step before connecting. `recommendationScreen` opens once, after measurement, with values computed for this route. It appears only with an interactive console and real evidence. Unattended runs use `--delay`-style flags or ini values, and still get the floors below.

Delay on one side and rollback on the other cover the same direction, so all the arithmetic is per-direction (`src/net/PacingBudget.h`):

```
client -> host coverage = clientDelay + hostRollback
host -> client coverage = hostDelay   + clientRollback
```

`classify` calls a budget `FullSpeed` only if it strictly exceeds the requirement; equal is `Marginal`. That is why every target below is requirement + 1.

| Value | Computed from | Used for |
|---|---|---|
| Comfort (`RouteRecommendation::required`) | p95 RTT: `oneWayFrames` plus the larger of 2 and a third of it (`safetyMarginFramesForOneWay`) | The recommendation and the on-screen "needs N frames" line |
| Floor (`floorFrames`) | Mean RTT: `oneWayFrames` only | The lowest value the UI and commit allow |

Frames round up at 60.6 fps. With a 40 ms p95 and 34 ms mean, one-way is 2 frames, comfort is 4 (target 5) and floor is 2 (target 3).

`recommend` produces two presets. `LowLatency` gives the receiver all the rollback its limit allows and delay only for the remainder. `Smoother` splits the target in half. With the example above and generous limits, that is delay 0, rollback 5 against delay 3, rollback 2. The screen opens on `LowLatency`.

The screen stops a player going below the floor, recomputed each tick. Your rollback floor uses the opponent's live delay from `MSG_SETTINGS_PENDING`; your delay floor uses their `maxRollback`, since rollback they might never use cannot be relied on. Falling short of comfort is allowed, and is shown in red with an explanation that both sides would run slow.

The floor is then re-applied at commit, because each side picked against a moving opponent. Order matters:

1. The client sends `floorFrames` in `requiredFrames`, then its `MSG_CONFIRM_CONFIG`.
2. The host takes the worse of the two floors with `worseRequirement`, raises its own values with `enforcedOwnRollback` and `enforcedOwnDelay`, and only then sends `MSG_ROUTE_COMMIT`. The commit must carry the final host delay, since the client's coverage depends on it.
3. The client raises only its own rollback using `commit.hostTuning.delay`. This is local and is not sent back, so the host's `cfg.remoteRollback` can understate it. Client delay cannot change after confirmation, which keeps the host's arithmetic valid.

`serviceSettingsHeartbeat` keeps the link honest during all of this: it pings every second and gives up after 10 s of silence. A player staying on the screen has up to 180 s on the peer's confirmation wait. Both values can be changed again during the match (`src/game/RuntimeDelayControl.h`).

`Marginal` still warns on the screen. It reads as "just fits", but coverage that only just fits has no jitter absorption, and a match played that way stalls at the prediction ceiling and loses speed on both sides.

**Why requirement + 1 matters.** Coverage that exactly fits does not degrade gracefully. A peer that hits its prediction limit stalls, produces its own inputs late, and starves the other peer, which stalls in turn. The pair settles below full speed with zero rollbacks, so the time is lost to waiting and not to correction work. The same mechanism is why `safetyMarginFramesForOneWay` scales with distance: a long route has already spent more of the window, so a flat margin that suffices on a short route falls short on a long one.

**Why the floors act the way they do.** Enforcement raises only a side's own values, and each of those serves one direction, so the two decisions compose without another round trip. `enforcedOwnRollback` is the first choice because rollback is a local ceiling that costs CPU headroom and never input feel, while raising delay adds lag a player may have declined. `enforcedOwnDelay` exists for the case rollback cannot reach: if the peer's rollback limit cannot cover a very long route, only this side's delay can. Both are raise-only and capped by that side's own limit. A route neither ceiling can cover keeps its warning.

The floor uses mean RTT because it forbids only what cannot run full speed on a spike-free link. The jitter margin and p95 tail belong to the recommendation, so a low but workable pick stays as chosen. `requiredFrames` carries this mean-based value, so it must never be read as a tail requirement.

`kCapabilityRollback` (30) is a real engine limit, tied to `kMaximumGameplayRollback` and the 31-slot savestate pool. `kCapabilityDelay` (15) is a product judgement: 15 frames is already a quarter second of input lag.

### Suspended launch and injection

`launchOnlineSynchronized` calls `prepareInjected`, which leaves a fully armed game with its main thread still frozen. The steps in `src/client/Launcher.cpp`:

1. **Refuse bad input.** `validateSessionDir`, `validateReplayLaunch`, `validateRecorderLaunch` and `validateSpectatorService` run here and again in the DLL, from the same header. Socket rules follow: online modes need a bound IPv4 UDP socket. A dedicated spectator socket is refused unless the spectator source runs in-process, where it must differ from the player socket in handle and port. Shipped launches never use that mode, since the sidecar keeps its socket in the caster. Then fresh admission and the drift check.
2. **Create the process.** `qinput::sdlReleaseForLaunch` runs first. The environment gets `QOH99_SKIP_INTRO`, `QOH99_NETPLAY_SESSION_DIR` and the ddraw override from `mergeDdrawOverride`; failing to build it is fatal only under Wine. `CreateProcessW` uses `CREATE_SUSPENDED`. `bindGameToCasterLifetime` puts the child in a kill-on-close job, so the game cannot outlive the caster.
3. **Create the rendezvous objects.** The names embed the child's PID, so they can exist only after step 2: `ipcReadyEventName`, `ipcFailedEventName`, the input pair, and `ipcPipeName`. The pipe is outbound-only, single instance with `FILE_FLAG_FIRST_PIPE_INSTANCE` so nothing can squat the name, and rejects remote clients.
4. **Share the sockets.** `WSADuplicateSocketW` fills `cfg.protocolInfo` for the child PID, and `repairDuplicatedUdpProtocolInfo` fixes blobs Wine leaves incomplete. This is the core reason the route survives launch: the game adopts the same bound socket with `WSASocketW(FROM_PROTOCOL_INFO...)` in `src/game/DPlayTransport.cpp`, so the NAT mapping just measured is the one it plays on. Both sides `udp.drain()` first, so stale probes do not reach the runtime.
5. **Inject.** `injectDll` writes the DLL path into the child and starts `LoadLibraryW` with `CreateRemoteThread`. A suspended process may not have mapped `kernel32` yet. `remoteModuleBaseOrValidatedCandidate` can then only offer a candidate base, so identity checking is deferred and repeated after the thread returns; a base mismatch fails the launch. On timeout with the thread still running, the argument page is deliberately not freed, since the loader may be reading it. The process is terminated next anyway.
6. **Hand over config.** `connectPipeWithTimeout`, then `writePipeWithTimeout` sends the raw `IpcConfig`. It is `#pragma pack(1)`, and `sizeof` is pinned by the contract. The cancel paths drain overlapped I/O synchronously because the `OVERLAPPED` is on the stack.
7. **Wait for Stage A.** `waitPhase` waits on ready, failed and process exit together. On failure the launcher pulls lines out of the session's `dll.log` with `readLogHighlights`, so the player sees the runtime's own reason.

Inside the DLL, `DllMain` does almost nothing under the loader lock: it starts `initFallbackThread`, which waits `kInitializeFallbackMs` and then runs `qoh99np_initialize`. The caster does not call that export, so this path is the one taken. `initThread` then reads the pipe (`readIpcConfig`), validates every field, adopts the socket through `startNetplayLink`, arms direct boot and the publishers, runs `installHooks`, and signals ready. Every mandatory failure signals the failed event rather than timing out. Hooks must be live before the main thread runs: the game's own network init must hit the hooked `CoCreateInstance`, not create a real DirectPlay object.

Each phase has its own deadline. `scaleLaunchTimeoutMs` triples them under Wine, capped at `kMaxPhaseTimeoutMs`, for cold prefixes.

| Phase | Constant | Base |
|---|---|---|
| Injection | `kInjectTimeoutMs` | 10 s |
| Pipe connect | `kPipeConnectTimeoutMs` | 10 s |
| Config write | `kConfigWriteTimeoutMs` | 5 s |
| Stage A | `kStageAReadyTimeoutMs` | 15 s |
| Stage B | `kStageBInputTimeoutMs` | 30 s |

`PreparedLaunch` is move-only and its destructor calls `abort`, so any early return kills the child. Its session-log writer lease is released only when Windows confirms the child stopped, which protects the folder from being archived while the game may still write to it.

**Why the Stage B deadline is long.** The managed input bootstrap in the game process advances as frames are dispatched, and the game blocks its dispatcher for several seconds while it loads. A controller path can take over 5 s even when nothing is wrong, while a keyboard-only launch finishes in about a second, so a short budget fails healthy pad launches. Stage B never waits for a missing device or a held control.

**Intro policy.** The launcher sets `QOH99_SKIP_INTRO=1` for everything except free play, which it identifies as `MODE_OBSERVE` with `directBoot` and `trainingEnabled` both zero. `directBoot == 0` alone is not enough, because spectator and replay launches clear it too. Online the movie is also a synchronization hazard: it is on no shared timeline, so two players would reach the title screen at different moments.

**Offline is not `MODE_NETPLAY`.** That mode installs the DirectPlay transport and sets the game's network mode, and with no peer the game hangs. Offline launches use `MODE_OBSERVE`, and the game runs as an ordinary local session.

**Module validation under Wine.** A COFF timestamp difference between the local and remote `kernel32` is logged and ignored. A builtin Wine module can report it differently from a file-backed mapping, and machine plus image size have already proved the layout, so treating it as a failure would break a working prefix.

**Replay ordering.** The broker's pipe name embeds the game PID, so it can only be created in the suspended gap, and `listenBeforeInjection` runs there. `brokerThread` must also be serving before `prepareInjected` returns: Stage A for a replay includes the full prebuffer, so serving afterwards would deadlock. A broker failure terminates the still-suspended process, and because the launcher waits on process exit as well, the phase ends immediately and does not sit out its deadline.

**Game files touched at launch.** The game reads `System\QOHcnf.key` during `WinMain`, before it ever calls `DirectDrawCreate`, so a wrapper-side edit would always arrive one launch late. The caster therefore normalizes two fields of the active profile before the process exists, replacing the file atomically and keeping `.prelaunch.bak`:

| Profile offset | Value | Why (native behaviour) |
|---|---|---|
| +108 | 1 | Selects the display path that calls `SetDisplayMode`, which the wrapper restyles into a resizable window. With 0 the game opens its own fixed window, and the wrapper's borderless mode and resizing do not apply |
| +376, bit 0 | Set | Full colour. The game draws its effects only with this bit set, so an 8-bit install shows no hit flashes and offers no in-game way to find out why. The other bits, such as NoSound and VSync, are the player's and stay untouched |

The file is 8044 bytes in the SE format: a dword for the active profile, then 764-byte profile blocks from offset 4. A direct launch of the game honours whatever is saved. `LocalConfig\FCommand.txt` has its own native rules. The game reads it only when a 10-byte `ConfigCheckFile.dmy` sits beside it, keys and values are case-sensitive, and the November tokenizer recognizes only CRLF separators, so the caster repairs line endings atomically before the modes that may use the native F-keys. `fcmd_ui` manages only known keys and keeps every other line as written. A key with a wrong-case value would be ignored by the game, so it is preserved as unknown and not reinterpreted. Training stays out of this path even though it is offline: it owns COM control, reset and HP, and the native F-keys drive the same state from outside its invariants.

**`--wine-fixture`.** This verb runs process creation, injection, the pipe, the events and socket duplication with the caster as its own child, and reports each phase separately, so a prefix can be qualified with no game. It resolves `kernel32` as the launcher does, by candidate and later confirmation, because enumerating a suspended child cannot work; on Wine the snapshot fails outright. `--fixture-child` stays alive for five seconds because the parent must still enumerate its modules after resuming it.

**Rules for changing `IpcConfig`.** Offsets of earlier fields stay stable, so new fields are appended or take over reserved bytes, and the DLL reads an exact byte count. Optional behaviour that leaves the layout alone can be a bit, as with `kReplayPlaybackSupersededStateSchemaBit`, which needs no version bump. That bit does not skip the state-proof comparison; playback still compares and logs, but a difference no longer counts as a failure. The caster sets it only when `SupersededStateSchema` is the sole mismatch.

Peer-local presentation fields stay out of `kRuntimeCompatibilityId`, because a HUD preference must not force both players to match. `hideNativeScore` and `hideNativeTimer` only gate cosmetic draw detours. `musicVolume` and `soundVolume` work because the game creates its DirectSound buffers with volume control and never calls `SetVolume` itself, so applying a volume through the buffer interface leaves the game's control flow alone.

`disableExtraFunctions` is not cosmetic. The game's hidden F2 to F9 commands write a hashed gameplay region non-deterministically, so they would desync a match as soon as either player's local input state moved, even if both agreed to allow them. Live network roles therefore ignore a request to allow them (`setNetplayExtraFunctionPolicy` only logs it), and `hkFCommandParse` applies the session policy.

**Secrets and identities in the handoff.** `recordChannelNonce` and `replayBrokerNonce` are minted separately, so a recorder can never authenticate as a playback broker or the reverse. `spectatorViewerId` comes from the caster's secure random source once and is never regenerated: preflight and the launched viewer are one session, sharing the socket, the generation and the header sequence space. `spectatorRelayTokenLow` and `spectatorRelayTokenHigh` are a capability and must not be logged. `sessionDirBytes` is validated and an over-long path fails the launch, because a silently cut path would send logs somewhere else.

**`secureRandomBytes` degrades, it does not fail.** The chain is `BCryptGenRandom`, then `RtlGenRandom`, then a labelled last-resort mix. Both DLLs are loaded at run time and probed with a real draw, so a Wine prefix with a stubbed `bcrypt.dll` falls through and the caster still loads. Modules are never freed, since unloading under a racing caller would be worse than keeping the handle. Callers that mint identity get a `degraded` flag and must log it. The last-resort mix guarantees distinct values but makes no claim of unpredictability.

### The resume barrier

With the game frozen, `launchOnlineSynchronized` starts the recorder and the spectator sidecar, then runs two barrier rounds over the control link: `MSG_RUNTIME_READY` (45 s) and `MSG_RUNTIME_START` (15 s). `validRuntimeSync` requires matching session, identity and cookie, the same route kind, and the opposite role. Only then does `resumePrepared` call `ResumeThread`. A slow machine therefore cannot leave its opponent running alone during load.

After resume, `resumePrepared` waits for Stage B. That is fatal online and only a log line for an offline training launch, where a late pad is tolerable. The control link closes unless the session paired through a relay, whichever route then won; the host releases its player link from the listener before spectator service starts reading it. From there `waitForLaunchedGame` is the sole reader of the host's control endpoint.

A failed spectator sidecar withdraws the spectator offer and never fails the match. `spectatorServiceMode` stays consistent because `prepareInjected` rejects a config where the source and mode disagree.

**After the game closes.** `waitForLaunchedGame` turns the child's ending into something a person can read. `qoh99ClassifyChildExit` treats native exit codes 0 and 1 as ordinary closes and passes every other status through exactly. It does not trust a clean code by itself, because the game exits with 1 on every path, including failures it has journalled. The caster therefore reads `QOHerror.txt` through `gameErrorJournalCandidates` and `readGameErrorJournalFile`, and `launchedAtFileTime`, stamped before process creation, keeps an old journal from being blamed on this run. It then quotes `[CRASH]` lines from the session's `dll.log`. A failed wait with the process still alive ends in `TerminateProcess`, so a game is not orphaned while the caster tears down its ownership.

**Keeping the shared address alive.** Interactive hosts return to the menu after the game closes, but viewers may still use the same address during retention. `RetainedHostControlOwner` moves the control endpoint to a below-normal worker before the host's stack unwinds. That worker calls `serveIntentDuringMatch` with no console, so Ask me first declines there and never opens a UI from the background. `retireRetainedSpectatorService` stops it before a new hosting round. The session log folder is compacted only after that service has gone and the log is closed. An archive failure leaves the folder intact, and an abrupt exit is handled by `recoverAbandonedSessionLogDirectories` on a later launch.

### Port mapping and the spectator port

`prepareSpectatorEndpoint` binds the spectator socket: the configured port if set, else game port + 1, else up to 8 ports above that, else an OS-assigned port. A busy port costs only a forwarding rule, never the session. `qnet::portmap::PortMapper` then asks the router to open it, on a worker thread so hosting never blocks:

1. NAT-PMP to the default gateway, port 5351, three tries in one second, and only a reply from the gateway counts.
2. UPnP: SSDP search for up to `kDiscoveryTimeoutMs`, then HTTP to the device. A description URL must point at the host that answered, and the control URL must stay on the router (`resolveControlUrl`).

Leases last `kLeaseSeconds` (one hour). `release` deletes the mapping from the destructor. **The player port is never mapped**; only `startSpectatorPortMapping` calls `requestUdp`. Failure just becomes a status line, since viewers can still arrive by punching.

`refreshSpectatorPortMapping` copies a successful external port into `SpectatorServing`, and it runs just before `hostNegotiate`. That function advertises the mapped endpoint only if the public IP is routable, because otherwise the LAN candidates already cover it.

Public address detection (`qclient::public_ip`) contacts `https://api.ipify.org/` then `https://ipv4.icanhazip.com/`, with override `QOH99_PUBLIC_IP_OVERRIDE`. It is advisory: `PublicIp.h` forbids choosing a route from it. It still produces the address shown to the player, `runAutomaticHost` derives the automatic relay room from that address, and `hostNegotiate` builds the spectator share address from it. When it fails and a relay is configured, `discoverObservedHostEndpoint` asks the relay what it sees instead.

**Lease limit.** A mapping is requested once for `kLeaseSeconds` (one hour) and never renewed: `kRenewIntervalMs` is defined but unused. A hosting session longer than the lease can lose its spectator mapping while the status still reads as mapped.

`buildSpectateAddress` will not combine the public IP with the locally bound port, because a NAT need not preserve the port and that pair would send viewers to a dead address for a whole race. A router-reported mapping is different, since the router states the external port, and that is why it alone can be advertised publicly. The address must also be routable: `validateCandidateSet` rejects a public-observed candidate on a private address, and one such entry would fail the whole set. One private slot is reserved for 127.0.0.1, so two casters on one PC can race loopback when the public address hairpins.

On some NATs the host's public control port aliases to the spectator mapping, so control datagrams arrive at the sidecar's socket. The sidecar remains the only reader of that socket. `openSidecarControlIngress` gives the listener a send-only duplicate, and the sidecar injects control-link traffic into it.

`qnet::portmap::wire` is pure and treats everything from the network as hostile, since any device on the LAN can answer. `findWanConnectionService` is a small scan and not a general XML parser, because only two strings of known shape are worth reading from an attacker-influenced document. The routing table picks the local address to name, because a machine with a VPN, a hypervisor bridge and a physical NIC has several plausible addresses and only one faces the router.

**Keeping public-IP lookup out of the way.** `src/client/PublicIp.cpp` depends on the prefix's certificates, proxy, TLS and DNS plus two outside operators, and all of those behave differently under Wine. So `wininet.dll` is loaded at run time, and a missing one disables the feature without stopping the caster from loading. The worker is detached, so a slow lookup cannot block Host cancel or process exit. That worker must not touch function-static logging state, which may already be in destruction during a fast shutdown; the menu thread reads the result and reports it.

### The console UI

`src/client/ConsoleUi.cpp` writes a real Windows console, so a frame is a list of rows rewritten in place, with each row's tail erased, and the screen is never cleared. Consequences for anyone adding a screen:

- **Rows clip.** That is right for tables and wrong for prose, which must go through `wrapDisplayText`. It counts display cells and breaks on character boundaries, so double-width Japanese text cannot be split or overfilled.
- **There is no scrolling.** Rows land at absolute positions. The Controls page is already 25 rows at worst in an 80x25 window, so one more row pushes the save row off the bottom. Its row count is also pinned by a test.
- **Translation happens at the edge.** `menu`, `message` and the frame path call `trText`, not the call sites. That keeps `tr` away from log lines, ini keys and wire tokens. Rows built by concatenation are translated in segments, so they need ordinary table keys and no code change here.
- **The font belongs to the console host.** Japanese needs a face with the glyphs, so `SetCurrentConsoleFontEx` tries MS Gothic and then fallbacks. Some hosts refuse, which is treated as a worse-looking menu, not an error. A log line records the refusal, because that case looks the same on screen as a missing font.

**Two backends, and Wine wants opposite ones.** In a Unix terminal Wine only approximates a screen buffer, so cell writes corrupt rows while VT works. Under `wineconsole` cell writes behave as on Windows and VT sequences appear as literal text. `SetConsoleMode` fails in both cases, so it cannot tell them apart. The test is `GetConsoleWindow`, which only the `wineconsole` case has, and `QOH99_CONSOLE_VT` overrides it.

**Retired settings.** `WritePrivateProfileString` only writes, so a removed key would stay in every player's ini indefinitely and keep describing a feature that is gone. `dropRetiredKeys` calls `removeKey` on each save; add a retired key there.

**Stale keystrokes.** The menu that follows a Training return has Host selected, and Training hotkeys include number keys, so `runOffline` calls `discardPendingInput` after Training returns. A leftover Enter would otherwise start hosting.

**Failures have two audiences.** `fail` always logs and always displays. `detail` is the player's half: cause and remedy, with no error codes, though a path is allowed because it is actionable. `technical` carries the rest, including `diagnosticContext`, and appears only in the expandable layer and the log. Put codes in `technical`, since a code like 10048 against 10061 is the whole diagnosis for a developer and means nothing to a player.

**Join notification.** `announceOpponentJoined` rings once each side knows who the other is, after the version, build and name checks, so a caster about to be refused stays silent. `RANYU.WAV` is the game's own challenger cue, but it is presentation content that an install may lack. The sound is read into memory and scaled by `quietedPcmWave` to `kJoinSoundLoudestDbfs`, not played by name, because `PlaySound` with `SND_ASYNC` reports success once it has queued a file it has not opened, which would leave the embedded fallback unreachable. The fallback is a `WAVE` resource and not a system alias, because a Wine prefix has no sound scheme. `SND_MEMORY` reads the buffer during playback, so any sound still playing is stopped before the buffer is replaced. `FLASHW_TIMERNOFG` keeps the taskbar button lit until the player returns.

### The Controls page

The page edits only the file the next launch compiles; a launched game keeps its snapshot. The controls file is therefore read fresh for every launch, so a rebind between two hosting rounds takes effect.

- **Browsing never destroys bindings.** Picking a pad with a different layout records a debt in `pendingStripProfileId`, paid by `payPendingStrip` just before the first new binding is committed. The order matters, since paying after the commit would remove the new binding.
- **A GUID is a device class, not a device.** Two identical boards share one, and a later binding edit writes the opened device's path back into the profile, so a wrong match would rewrite the saved identity. Reconnecting uses `resolveDeviceIdentity`, the same resolver the game uses, and refuses to guess. When two boards cannot be told apart by any persistent key, the page asks the player to press a button on the one they mean, because the row's "#1" ordinal is only an enumeration position that the game process may see in a different order.
- **Migration only when unambiguous.** Steam Input, remappers or a new USB port can replace a bound device's identity with no error. If exactly one enumerated device can honour the profile's bindings, the profile moves to it and the page says so (`[controller.migrate]` in the log). Otherwise it does not move.
- **Save refusals must not trap the player.** Esc means save and back, so a refused save offers fixing the binding or leaving without saving. The whole file is validated, so `blockingPlayer` names which player's profile refused, and the page offers to go there, because a rebound profile's name may still say "keyboard". Player 1's profile must always compile. Players 2 to 4 are validated only once every action is bound, and launch skips an incomplete one. Global mode controls belong to local P1.
- **SDL startup is slow and belongs off the UI thread.** The first gamepad init enumerates HID synchronously, which is milliseconds for an XInput pad and several seconds for a DualSense. `sdlWarmupAsync` runs it when the page opens, and `sdlReleaseForLaunch` also waits for any warm-up still in progress, so the caster owns no gamepad state once the game does.
- **Device logs.** The candidate list is logged each time it is built, on one line with the backend and SDL bootstrap result, because that is the first thing a failed selection needs. Hardware paths and serials are hashed there, since players post these logs.

### The `relay` verb

`qoh99caster relay` runs a self-hosted server and needs no game. `main` passes it to `qrelay::runRelayVerb` in `src/relay/RelayServiceVerb.cpp` before `parseArgs`, and it skips the antivirus preflight. With the CMake option `QOH99_BUILD_LEGACY_RELAY_SHIM`, which is off by default, `src/relay/RelayServer.cpp` builds a standalone `qoh99relay.exe` that calls the same function after printing a deprecation notice.

`parseRelayOptions` produces a `RelayServiceConfig`, `RelayService` validates and binds it, and `SetConsoleCtrlHandler` routes Ctrl-C to `requestStop`. Results are `ExitCode` values, so a script can tell a usage error from a bind failure. It serves TCP pairing and raw UDP on one port, `kDefaultRelayPort` (14764), and reserves player, spectator and topology capacity separately, so viewers can never use up the player lane.

| To do this | Use |
|---|---|
| Run a relay | `qoh99caster relay --bind 0.0.0.0 --port 14764` |
| Use it automatically | Set `QOH99_RELAY_OVERRIDE=ip[:port]` on both casters |
| Pair explicitly | `host --relay <ip[:port]>` and `join --relay <ip[:port]> --room <32-hex>` |
| Diagnose | `--force-relay`; `--rendezvous-only` stops after route commit, before any process exists |

Relay addresses must be numeric, since `Endpoint::parse` does not resolve names.

**Lanes.** One UDP port carries separate forwarding domains, and each datagram must pass its own inspector: `inspectPlayerDatagram`, `inspectSpectatorDatagram` or `inspectTopologyControlDatagram`. Each checks magic, CRC and payload shape and rejects the others' traffic, so the server cannot forward arbitrary UDP. A new player-lane message kind must also be added to the player inspector, or it is dropped as invalid on relayed routes only. A missing `SessionEvent` case, for example, would break pause and rematch for relayed players while direct play kept working.

The player lane forwards raw QNP because a room has one destination. A spectator source fans out to many viewers, and several QSP records name no viewer, so spectator traffic gets a `kSpectatorFrameHeaderSize` (24-byte) outer header for routing. The relay validates the frame and forwards it byte for byte. The header authenticates nothing: as on every lane, that job belongs to the registered UDP endpoint binding.

**Record families.** The 16-bit version selects a record family. `kSpectatorVersion` and `kObservedVersion` are both 3, with disjoint kind ranges, so the service demultiplexes by kind and shape. A version check alone would let the first branch swallow every `ObservedRequest`. The observed answer goes only to the endpoint that authenticated, echoing only that role's token, so the other peer can neither read nor forge it. `relaySupportsObserved` false means no authoritative answer, and the caster must say so and not treat a zero endpoint as success.

**Fairness and lifetime.**

- `receiveUdp` scans twice `maximumUdpPacketsPerTurn` plus `maximumUnboundPacketsPerTurn` datagrams, and each lane has its own allowance. A spectator burst queued ahead of player input therefore cannot hide that input behind a shared limit.
- The account a packet is charged to comes from the authenticated endpoint binding, never from packet contents.
- Player and spectator lifetimes are independent. When a player control stream ends, both player leases go and the room becomes `retainedSpectatorOnly` until its source closes or misses a refresh. The topology lane ends with the player session, since its authority is the live host.
- `spectatorRegistrationTimeoutMs` is short and separate because a viewer takes capacity at TCP Welcome, and idle leases must not be able to pin every slot.
- An address's rate bucket outlives its last connection, or a connect-close loop would get a fresh burst each time. `maximumTrackedIpStates` caps that table against spoofed sources.
- `elapsedSince` guards every timeout. A service turn captures `now` once while `receiveTcp` stamps with a later clock read, and plain unsigned subtraction would wrap and close a healthy room as idle.

**Grants and logs.** Expired, wrong-viewer, wrong-session and replayed spectator grants all return `SpectatorStatus::GrantRejected` and differ only in metrics, so a prober learns nothing about which field it guessed wrong. Topology admission runs over UDP and has no delivery guarantee, so an exact retransmission of an already redeemed grant at the same generation returns the same lease, not a replay refusal. Tokens, grant MACs, keys and cookies are never formatted into a log line. Room ids are join capabilities too, so `roomTag` logs a salted 32-bit tag.

### Session log archive and retention

`src/client/SessionLogArchive.cpp` compacts a finished session log folder into one ZIP. A session stays an ordinary writable folder until every known writer has exited, so a slow disk or failed compression cannot affect the session's diagnostics. An unexpected kill cannot run cleanup, so crash and desync evidence survives by default.

**Markers decide who may touch a folder.** A writer lease is a small marker inside the folder, recording PID and process start time, and it is left behind on purpose if the owner dies. A later caster archives only after proving that the original process is gone. `acquirePendingSessionLogWriterLease` brackets `CreateProcessW` in `src/client/Launcher.cpp`, because the caster could die after creating the game and before its PID is known. The pending marker reads as `ActiveOrUnknown`, which keeps that folder indefinitely instead of risking a running game's evidence. A folder with no marker because creation failed simply stays unarchived. The archive itself takes its own lease before any work, so a second caster starting mid-archive defers.

**Verify before delete.** The archive is written to `.zip.part`, then `verifyArchivePart` decompresses each entry and compares it byte for byte with the source file. Only then is the folder removed. Any failure deletes the part file, keeps the source and leaves a note through `writeArchiveFailureNote`. The work happens in-process through miniz, so it behaves the same under Wine. The caster deliberately spawns no script for this: a hidden shell doing compress-and-delete from a binary that also injects a DLL looks like malware to antivirus products,. ZIP entry names keep miniz's default UTF-8 flag, since session paths can be non-ASCII.

**Retention.** `pruneAgedSessionLogs` runs with `kSessionLogRetentionDays` (2) and is narrow on purpose:

- Only names starting with `session_` are considered, so anything else in the folder is safe.
- A folder with a live or unknown marker is skipped.
- Age is the newest last-write in the whole tree (`newestWriteBelow`). A directory's own stamp moves only when an entry is added or removed, so a long session's folder would otherwise look older than it is.
- If enumeration fails, the age counts as unknown and the folder stays.
- `maxAgeDays == 0` disables pruning and does not mean "delete everything", because zero is what a mis-parsed setting produces.

### Join sound

`qclient::quietedPcmWave` in `src/client/WaveGain.h` prepares the opponent-joined cue in a private copy. `PlaySoundW` has no volume, and the system's wave-out volume call affects the whole audio session on Windows and behaves differently under Wine, so the samples themselves are scaled.

The two possible sources are far apart in level: the game's own `RANYU.WAV` is music mastered hot, and the bundled fallback is much quieter. A fixed gain would leave one too loud or make the other whisper. So `loudestWindowRms` measures the loudest 400 ms and `gainForLoudness` brings it down to `kJoinSoundLoudestDbfs`, and never raises a sound already below that level. Output is always 16-bit, because reducing an 8-bit source and writing it back as 8-bit would leave an audible hiss. Anything other than plain 8- or 16-bit PCM returns an empty result, so `announceOpponentJoined` moves to a sound it can turn down.

### Unused frontend modules

The menus run on `ConsoleUi`. `src/client/SdlUi.cpp` and `src/client/RawInputCapture.cpp` are compiled and tested through `src/client/tests/test_sdl_ui_model.cpp`, but no production code constructs `SdlUi` or `RawInputCapture`. Anyone wiring them in must keep these constraints:

- **SDL3 is delay-loaded.** Touching a symbol before a plain `LoadLibrary` succeeds kills the process with an entry-point error. Failure returns a reason so the caller can stay on the console.
- **Gamepad ownership.** `qinput::sdlBootstrapOnMainThread` refuses to run if someone else already initialized SDL's gamepad subsystem, and the player silently loses controller input. Call `releaseGamepadNavigation` before `qinput::sdlWarmupAsync` and before every launch.
- **Fonts.** No font is embedded, because the project has no redistribution licence for a Japanese face. `FontProbeResult` reports what GDI found, and switching language has to rebuild the font, because a preferred Latin face draws Japanese as boxes.
- **Raw Input.** `wVirtualScanCode` is a console contract that Wine, IMEs and non-US layouts translate or zero, so a rebind needs the raw make code. A message-only window is never in the foreground, so registration uses `RIDEV_INPUTSINK` and each packet is filtered by `processHasForeground`. DirectInput needs a real top-level window, hence the separate hidden `focusWindow`. The owning thread must never be killed with `TerminateThread`, since that corrupts its window class and registration; leak the handle instead. Pause arrives as E1 1D 45, so the trailing 45 must not be read as NumLock.
- **Clipboard.** Allocate and fill the global block before opening and emptying the clipboard, so a failure never destroys what the user already had.

## Spectators

A spectator does not watch video. It runs its own copy of the game and replays the confirmed inputs of the two players, checking its simulation against state hashes taken on the host. Everything in this subsystem serves one rule: spectating must never affect the players. A failure degrades or ends the spectator service, and the player match continues.

### Three parts in two kinds of process

```
 HOST MACHINE                                        VIEWER MACHINE
 game process            caster process              caster            game process
 ------------            --------------              ------            ------------
 GameHooks.cpp           SpectatorSidecar            SpectatorRoute    DPlayTransport.cpp
 SpectatorPublisher  --> SpectatorArchive            (preflight, then  SpectatorViewer
   local named pipe      SpectatorSource             socket handoff)   GameHooks.cpp
                              |                           ^            playback policy
                              |  UDP on the spectator port (QSP)            ^
                              +---------------------------------------------+
```

| Layer | Files | Job |
|---|---|---|
| Publisher | `src/game/spectator/SpectatorPublisher.*` | Runs in the game. Turns confirmed frames into framed messages on a local pipe. |
| Sidecar | `src/client/spectator/SpectatorSidecar.*` | Runs on a worker thread in the caster. Owns the spectator socket, admission, the archive and the viewer fan-out. |
| Session core | `src/spectator/SpectatorProtocol.*`, `SpectatorArchive.*`, `SpectatorSession.*`, `SpectatorCheckpoint.*`, `SpectatorSetArchive.*`, `SpectatorGrant.*` | Wire codec, chained archive, `SpectatorSource` and `SpectatorViewer` state machines. No sockets or threads. Built as the `qoh99_spectator` library. |
| Route | `src/client/spectator/SpectatorRoute.*` | Viewer-side preflight in the caster: finds a working path before any game is launched. |
| Playback | `src/GameHooks.cpp`, `src/game/SpectatorPlaybackPolicy.h` | Decides each frame whether the viewer's game advances, holds or fails. |

The host's game process never touches a viewer. Because all viewer traffic lives in the caster and on a separate socket, a flood of spectator packets cannot reach the player simulation thread.

### Data flow on the host

`GameHooks.cpp` publishes from the confirmed-frame path when `g_spectatorServiceMode == 2u`. For each frame it builds a `ConfirmedRecord`: the record index, timeline frame, both canonical inputs, and `stateHash`, which is the state hash *before* that frame ran.

The game enforces two conditions before publishing:

- `confirmed.frame` must equal `g_spectatorSidecarRecord`, so records are contiguous and a record index is a timeline frame.
- Both pre and post hashes must be nonzero.

If either fails it calls `publishFault` with `PublishDisableReason::InvalidRecord`. The service degrades; the players do not notice.

After each record, `publishTailProof` stores the post-state hash of that record. The worker sends the newest stored hash, without sealing, once no record has been written for `tailProofIdleMs` (40 ms). Three other things travel on the same pipe. Only set events are split into the record batches by cursor; a checkpoint goes after the turn's batch, and a seal waits until every record before its cursor is written:

- `publishChainedSetEvent` carries set events: match started, palette selected, match result, returned to character select (`SetEventKind`). Match boundaries are detected in `spectatorSetEventTick`.
- `publishStateCheckpoint` carries full-state checkpoints.
- `publishSeal` ends the stream.

The publisher writes on its own worker with bounded options (`SidecarPublisherOptions`): two 6 MiB checkpoint slots, a 1 s write timeout, 6 s of reconnect budget. Overflow or a dead pipe latches a fault through `latchPublicationFault` rather than blocking the game. Frames are defined twice, in `SidecarMessageKind` and `PublicationWireKind`, and those two lists plus the header layout comment in `SpectatorSidecar.h` must stay identical. `kPublicationMagic` and a CRC on header and payload guard the framing.

**Set events share the record chain, so the game thread folds them.** `publishChainedSetEvent` stamps each event and folds it into `producerPrefixDigest` on the game thread, at the position the next record will derive from. From that moment the event is part of the canonical chain and losing it would fork the digest, so the space check comes before the fold, never after. An event stamped at cursor C must reach the sidecar before record C, because record C's digest already derives from it. The worker therefore splits record batches at event boundaries and does not reorder around them.

**The two kinds of publication fault drain differently.** A failed pipe write can lose a batch already popped from the ring, so no later record can repair that prefix. Every other fault (`PublishDisableReason`) leaves the ring intact, and the worker drains exactly the accepted prefix before sending the fault. A clean seal may sit behind more than one worker turn, so the whole ring drains through the frozen terminal cursor first, with no reconnect during teardown: a pipe that is already gone is itself a visible truncation at the caster. The first requested terminal never changes, but a pipe failure after it stops it from going out as a clean completion.

**Connect attempts are spaced on purpose.** `connect` returns at once while the caster's pipe server does not exist yet, and the worker turns every `idleSleepMs` (1 ms). Without `reconnectBackoffMs`, all of `maxReconnectAttempts` would be spent in the few milliseconds between the DLL finishing init and the caster arming the sidecar, and publication would disable itself.

**Why the publisher looks the way it does.** `publishConfirmed` runs on the authoritative simulation thread, so it must cost the player nothing: one fixed-size copy into a slot of the preallocated ring (`kConfirmedRingCapacity`) and a release store. It takes no lock and does not allocate, wait, log or copy history. The pipe, framing and writes all live on the worker. A consumer that falls behind disables publication for that session, explicitly and with a reported reason, and the match continues unchanged. This module owns no viewer socket and does not interpret checkpoint bytes; regions reach it as opaque payloads.

**Reconnect budgets.** The worker gives up on the pipe only when both `maxReconnectAttempts` and `reconnectDeadlineMs` are exhausted. Connecting fails instantly while the caster's pipe server does not exist, and the caster arms the sidecar only after the DLL signals it is ready. Attempts alone would burn out in milliseconds, so `reconnectBackoffMs` spaces them and the deadline is the budget that actually governs.

**Who may end the stream.** The first `publishSeal` claims the seal, and later calls do nothing. It snapshots the published cursor and prefix digest at the call, the worker drains every record up to that cursor, and only then writes the seal. A latched fault suppresses a clean seal for good. A fault other than `ChannelFailed` still drains the accepted prefix before the fault frame goes out, which is what lets the sidecar truncate at a provable cursor.

### What the sidecar guarantees

The sidecar applies publications in `SpectatorSidecar::publish`:

- **One-record hold.** `applyRecord` keeps the newest record pending until a proof arrives for it, either the next record or a tail proof, then `flushPendingRecord` commits it. So the archive only ever exposes records whose resulting state is proven.
- **Rolling digest.** The archive chains `rollingDigest` through every record and set event. A resume point names a cursor and a digest, not just a cursor.
- **Terminals have one answer.** `requestSeal` carries the authoritative record cursor and prefix digest. `rememberTerminalRequest` keeps the first and counts disagreement in `conflictingTerminalRequests`. `exactTerminalPrefix` checks the claim against the archive.
- **Truncation is honest.** If the publisher dies or faults, `sealTruncated` ends the stream with `TerminalReason::ProtocolFailure` at the last provable cursor, dropping an unproven pending record and counting it in `truncatedRecords`. Viewers see a clean end of what could be proven, never a guess.

After the game exits, the caster calls `retainSidecarAfterGame`. `drainDeadlineMs` (8 s) lets connected viewers finish; `retentionMs` (120 s) keeps the archive for late joiners. `SidecarPhase` tracks this: `Sealed`, `Draining`, `Retained`, `Expired`. `admit` refuses new viewers during `Sealed` and `Draining`, and a retained archive can still serve one. Retention is bounded by `maxRetainedRecords` and `maxRetainedBytes`. `firstAvailableRecord` reports the floor if old records were dropped.

`SpectatorSource::pollOutbound` is the transmit side. Viewer ACKs carry a record cursor, an event cursor and a digest. Records and events are repaired separately, since a viewer stalled on a missing set event keeps asking for it (`recordRepairSends`, `eventRepairSends`). All source-to-viewer datagrams share one sequence counter because the viewer has one 64-entry replay window (`kSequenceWindowEntries`). The same one-counter rule covers a socket handoff: the viewer's game continues at `spectatorSequenceBase`, the next unused sequence after preflight, since restarting at 1 would replay spent numbers.

**Three rules hold up the service.** The sidecar never invents a post-state hash it does not hold. Spool corruption or exhaustion ends the spectator service and changes nothing a player can observe (`failService`). And ingress token buckets run before any decoding, with logging time-windowed and byte-capped (`CoalescedDiagnostics`), so hostile traffic can grow neither the cost per packet nor the log. The overflow bucket used for unseen addresses once the address table is full is configured once for the service's life. Reconfiguring it per address would hand every spoofed packet a fresh burst.

**A set event is never dropped for arriving early.** The records after an event declare it in their `baseEventIndex`, and a viewer refuses to fold in a record ahead of an event that precedes it. A lost event would therefore wedge every viewer for good, with the source repairing records forever because it has no event to send. `applyChainedSetEvent` puts each event in `_heldSetEvents`, including one that arrives before the match descriptor, and `drainHeldSetEvents` appends it when its exact boundary commits. The archive frontier can lag the wire by the one held record, which is why the queue is needed at all. A gap, a digest mismatch or an overflow of the 32 slots is real chain disagreement and fails the service like a corrupt record.

**Silence must not become a clean ending.** A second publication Hello or a frame-sequence gap becomes `PublicationKind::LossReport`, so producer loss is never later sealed as a completion the sidecar cannot prove. End-of-pipe only counts as loss once every queued event has been consumed: a terminal can sit behind more than one turn's `kPublicationDrainBudget`, and sealing early would overtake it and falsely truncate an exact stream. The pipe reader publishes its reconnect intent before dropping `connected` so the service thread never sees a false/false pair. `retainSidecarAfterGame` gives the publisher `kPublicationReconnectGraceMs` to report its own terminal, and only then synthesizes a truncation. It returns at once on `SidecarPhase::Failed`, so `failService` must close the relay registration itself.

**Why new viewers wait out the drain.** The sealed-tail drain belongs to viewers that already hold a prefix. A new arrival would gain nothing from the tail and would re-pin a slot and a handshake deadline, making the caster wait out the whole drain budget. The foreground game-close handoff blocks for at most `drainDeadlineMs`, the same budget the service enforces. `Retained` admits again because watching a finished match from the start is a feature.

**Viewers are keyed by identity, not endpoint.** `_viewers` uses the stable viewer id, so a NAT rebind or a restarted process keeps its slot instead of becoming a ghost second viewer. The source addresses outbound packets by route token, so per-viewer accounting has to resolve across the two key domains. Pre-decode ingress quota follows the authenticated relay viewer or direct address, while admission capacity may be pinned to the real address the control grant observed. A migration comes from a token not yet mapped, so it is charged to the existing grant before the costly core authentication.

**Answers a viewer depends on:**

- A Reject must echo the viewer's `requestedGameId`, because the viewer discards a mismatch. A wrong value silently removes the only channel that can tell a refused viewer why.
- An admitted Hello that creates no source state is the normal pre-game wait, so `onDatagram` answers it with `sendHeartbeat`. A viewer held for approval gets the same answer on each retransmit. Without it, "the match has not started" and "this host is unreachable" look identical from the viewer's caster.
- A Handshaking viewer's deadline restarts on each accepted same-generation Hello, because the game adopting the socket says Hello seconds after the preflight did. `kHandshakeRefreshCapMs` caps this so retransmits cannot pin a slot.
- `kHelloFlagWantsNewestCheckpoint` is read in the sidecar's own Hello decode and forwarded out of band. After a socket handoff that Hello can be a same-generation retransmit whose sequence preflight already spent, and the source's ingress would discard it before reading the flag.

**Reading the logs.** `logViewerSummary` writes one line per viewer (route, state, acked cursor, backlog, repair sends, progress and contact ages) plus traversal counters, every 5 s with viewers and every 30 s without. It is the host-side line to read against a "can't watch" or "it stutters" report. A rejected ACK is the case to look for, since the source then never learns what the viewer is waiting for, which is why ingress results are logged by name. Grant ids, MACs and cookies never reach a log line; `redactedTag` gives a short non-reversible tag for correlating lines.

**Archive bounds.** Records live in fixed-capacity segments, so an append never reallocates and copies history. Allocations on the publishing path do not throw: exhaustion returns `ArchiveResult::AllocationFailed`, `serviceDisabled` latches, and the sidecar fails the spectator service while the game carries on. Retained history stays readable.

Retention can be held back by a resume floor (`setResumeFloor`) or an advertised checkpoint, but only up to `maxRetainedRecords`. Past that, `retentionBlocked` latches and the stale floor and checkpoint references are dropped, because keeping them would let one slow viewer grow the source without limit. That viewer then gets a `Reject` with `RejectReason::ResumeMismatch` when its acknowledged cursor falls below `firstRecord`. Only the newest checkpoint's encoded blob is kept per game (`setNewestCheckpointBlob`), since it is the only one ever offered.

### The QSP session rules

These rules live in `SpectatorSession.cpp`. They are easy to break because each looks like an over-strict or redundant check.

**Identity, not address.** Viewers are keyed by a stable random `viewerId`, and every viewer-to-source message names it. It must come from a secure random source (`secureRandom64`), since a predictable id lets an off-path sender guess a live identity. A Hello from a different endpoint is refused; changing address takes an authenticated `Migrate` (old generation current, new generation strictly newer, resume digest matching). The 128-bit cookie is an off-path capability and the CRC only detects damage. Neither authenticates a viewer; `SpectatorGrant` does that.

**Proof before the replay window.** `handleHello` admits a sequence number only after the resume digest has been checked. A forged Hello therefore cannot evict a viewer or poison its window. The all-zero resume is allowed without a digest only for an unknown viewer or a retransmission of the same generation, because a first handshake does not yet know the descriptor-derived seed. Any newer generation must prove its prefix. `handleDetach` has a similar narrow exception: a zero-cursor Detach from a slot still in `AwaitingInitAck`, with nothing sent, is accepted so a route attempt can release its direct slot when the viewer never learned the seed.

**Early and pinned requests.** A valid Hello that arrives before any game exists gets `Accepted`, creates no state and sends no reject, since viewers may legitimately start before the players finish their ready barrier. Once a `kLatestGameId` request resolves, retransmissions in that generation stay pinned to the chosen game, so a late duplicate cannot drift to a newer one.

**Reject is not handshake-only.** The source also sends it after admission, when the archive disappears or rotates past the viewer, so the viewer honours it while streaming. The viewer accepts `kLatestGameId` as a wildcard for the game field, because a host with no bound game has nothing more specific to say. The viewer id, generation and nonce must still match exactly. Requiring an exact game echo would make a viewer ignore refusals and hang.

**Acks that fail verification still carry information.** A viewer blocked on a missing set event holds its records and truthfully acks a record and event pair the source would never have produced, so the digest cannot match. `armRepairsFromUnverifiedAck` uses those cursors to arm fast repairs of both streams before returning `AckDigestMismatch`. They never advance the acknowledged frontier, since an unverified cursor may not retire anything. The digest is a consistency check here; cookie, sequence window, viewer id and generation have already authenticated the datagram.

**Repair timing.** `retransmitMs` (350 ms) sits just above a 300 ms round trip, to avoid needless repairs on a healthy slow path. Three duplicate acks at an outstanding frontier (`duplicateRecordAcks`, `duplicateEventAcks`) request an earlier, rate-limited repair. The timer remains the fallback for a lost final packet, which produces no later ack.

**Packet budget order.** `pollOutbound` serves work in this order, and the order matters:

1. Rejects and per-viewer control, up to `controlPacketBudget`. Set events and terminals are in this pass, so a full data backlog cannot starve a 33rd viewer's `CapacityReached` or hold up an event that records are waiting behind.
2. Checkpoint push. It runs before record data because a catching-up viewer can otherwise fill every budget with record batches, and the offer would never go out while that viewer is holding its playback for it.
3. Record data, up to `globalPacketBudget`.

The control and data passes keep round-robin cursors that resume at the first unvisited viewer, so backlogged viewers are served in fair blocks and not in a sliding window that favours the first few.

**Events gate records.** A viewer will not fold a record into the chain until it has every set event before it (`baseEventIndex`). A dropped event therefore stalls instead of silently diverging the digest. `awaitingEventIndex` remembers which event is missing.

**Diagnosing a stuck viewer.** On the viewer, `awaitingEvent=` in the freeze and link lines is nonzero when an event is missing. On the host, the sidecar's viewer line prints `repairs=` as record/event counts and `unverifiedAcks=` with the last cursors. Many record repairs, no event repairs and growing unverified acks point at a starved event, not a bad network.

**The end of a stream.** `TerminalAck` binds `postTailStateHash`, the state after the last record, which is not the last record's pre-state hash. An empty game's post-tail state is the descriptor's initial state. The viewer sends the ack only after its own playback reaches that state. `commitPlaybackRecord` checks each record's pre-state and remembers the post-state, and it does not compare neighbouring records, because adjacent records can use different hash profiles across a gameplay or layout boundary. Once a terminal is accepted the viewer has everything it needs: `resume` and `reportTransportFailure` both leave it alone, and it finishes from local history even if the source vanishes. Re-handshaking there would discard the only post-tail proof.

### Set events

A connected set is an immutable manifest plus an ordered, typed timeline positioned on the confirmed record cursor (`SpectatorSetTimeline`). Each event is folded into the same rolling chain as records, so dropping, reordering or inserting one breaks the chain like tampering with a record. Viewers only verify and replay events; `SpectatorViewer` has no way to originate one.

Events describe the session and do not steer the simulation, because scene changes should arise from the recorded inputs. The one exception in `GameHooks.cpp` is `ReturnedToCharacterSelect`, which the viewer executes as a dispatch return of zero when playback is exactly at the event's record and the scene is still a match. This keeps it in lockstep after an F1. A viewer already past that record, or out of the match, logs and skips it.

`PaletteSelected` is presentation only. It is chained like any event but never reaches the state hash, and exists so that a viewer sees, and a saved spectator replay keeps, the colours the players used. It names exactly one slot (`activeSlotMask`) and the open match, and `checkOrder` refuses anything else, so colours from a finished game cannot reach the next one in a set. Only the variant in use travels, 64 bytes, to keep the event small. `DelayChanged` is metadata as well: an in-match delay change does not alter the confirmed actions a viewer applies.

**Binding mid-stream.** A viewer restored from a checkpoint never saw earlier events. `bindAt` treats everything below the base as elided by proof, since the checkpoint's digest attests it, and a retransmitted copy is a harmless duplicate. `BindSeed` seeds the open match, match count and pause state from the boot descriptor; without it the order rules would refuse every event of the in-progress match as `OrderViolation`. For the same reason a match is reproducible on its own only if `independentBootstrap` is set: the first match boots from the manifest alone, and a rematch needs the set prefix or a verified checkpoint.

### Admission and grants

Admission has two stages, and a viewer normally passes both.

**Control stage.** The viewer's caster contacts the host's control endpoint and sends a `ClientIntent`, handled by `serveIntentDuringMatch`. `INTENT_STATUS` gets public presence only. `INTENT_SPECTATE` is judged by policy:

| Setting text | `AdmissionPolicy` | Effect |
|---|---|---|
| "Anyone with my host address" | `Public` | Grant issued; a Hello with no grant is also admitted. |
| "Ask me first" | `AskHost` | Console prompt on the host. A caller with no console declines. A Hello with no reservation is denied `GrantRequired`. |
| "Nobody - do not open a spectator port at all" | `Disabled` | No port, no secrets in the answer. |
| (not offered in settings) | `CodeOnly` | Used only by the join-side collector described below. A stored value of 3 loads as Ask me. |

The caller first asks with `sessionId == 0` to learn the session, then asks again with that session, and only that second call reaches `sidecarIssueGrant`. Only `CodeOnly` compares the key; `Public` and `AskHost` need just a nonzero session id.

**Datagram stage.** The UDP Hello reaches `SpectatorSidecar::onDatagram`, which applies global, per-address and per-grant token buckets (`RateLimitConfig`), then `admit`. For a viewer holding a reservation, `admit` requires all of:

- not revoked, declined, expired or already consumed;
- the Hello's resume cursor and digest match the grant;
- a relay grant arrived through relay ingress and a direct grant did not;
- `reservationGrantValid` accepts the MAC;
- capacity and `maxViewersPerIp` hold.

A `SpectatorGrant` is a bearer token: HMAC-SHA256 truncated to 128 bits over the canonical body from `encodeGrantBody`, covering viewer, session, expiry (300 s from the control stage), source generation, route and resume point. For direct and punched routes `sidecarConfigFor` generates a fresh key per launch with `secureRandom64`. Relay-route grants use the key the relay issued to the source (`verifyRelayGrant`). The shared viewer cookie in `SessionIdentity` is deliberately not the MAC key, since every viewer knows it.

Grants are single use, with one exception. If the sidecar's own no-progress check in `enforceDeadlines` drops a viewer, `armReattach` keeps its pass redeemable for `kReattachWindowMs` (60 s), with a forward resume point accepted only when `resumePointIsOnOurChain` confirms it. Without this a viewer dropped by the host would be refused its own retry. A viewer that `SpectatorSource` retires first, as `Idle` or `NoProgressDeadline`, takes the retired path in `enforceDeadlines`, which does not arm re-attach.

**Pending approval.** Under Ask me first, a reservation not yet approved returns `AdmissionDecision::Pending`, which gets a heartbeat and not a reject. The console prompt runs inside `serveIntentDuringMatch`, so it blocks that control loop while it is open.

### Spectator route selection

`preflightSpectateWith` runs in the viewer's caster before launch:

1. Control exchange, which returns candidates, the grant and the observed public address.
2. A race of Hellos over LAN and public candidates (`directRaceMs`).
3. Hole punching via the punch service. The host's sidecar also looks the viewer up and punches first, through `configureSourceAutopunch`.
4. If a traffic relay is available, `relayReserve`, with `kRelayLaneReserveMs` of the budget held back for it.

A definite failure (`isDefinitivePreflightFailure`) stops the attempt; an inconclusive one still launches the game, which holds at the rendezvous, unless `--spectator-route relay` forced the route. Success hands the proven socket to the game through `IpcConfig`, with the route and grant fields.

### Host and relay behaviour

The host's sidecar is the only source in ordinary service. It fans out to up to `maxViewers`, limited to `uploadBudgetBytesPerSecond` (3 MiB/s) with per-viewer unacked limits.

The relay lane exists only when someone runs `qoh99caster relay` and the player match itself went through a relay. `sidecarConfigFor` fills `SourceRelayConfig` only for the `RelayRole::Host` side with a valid `spectatorRegistrationToken`. The relay forwards opaque QSP datagrams. It holds no archive. It generates each lane's grant key, hands it to the source in `SpectatorWelcome`, and checks relay viewers' grants against it. Relay viewers appear to the sidecar under `kRelayVirtualIpv4BE`, and per-address quotas use `ingressQuotaAddress` so that they do not all count as one address. `RelayViewerAdapter` handles registration on the viewer. `SidecarTransportEventKind` reports source registration state. The public punch service carries no match traffic and is not a relay.

**One socket, one reader.** Source-side punching runs on the exact socket that serves QSP, because a second helper socket would advertise a NAT mapping no viewer traffic ever uses. `UdpSidecarTransport::recvFrom` therefore consumes Mapping and Punch datagrams first, up to `kTraversalConsumeBudget` per call, before anything counts them as QSP. A NAT can also reassign the host's advertised public port to this socket, so control-link frames meant for that port may land here. Only exact control-link types are removed, through `queueSidecarControlDatagram`, and `HostControlEndpoint` stays the sole authority on them. The source asks about the viewer's spectator port, which names its real mapping and makes the service notify the viewer about the source. It also sends one datagram straight at the known address, which is enough on a full-cone or address-restricted NAT but not on a port-restricted one.

**Relay ingress is already authenticated.** Under `Public` a relay viewer needs no host reservation: the relay has already redeemed the code's bearer token into a per-viewer lease, and a player-relay room has no host control listener to reserve through. Restrictive policies still require the route-bound reservation (`admit` denies `GrantRequired`). Direct reservations are bound to the observed source address, but relay packets arrive through the virtual address, so cancellation does not compare that address to the control peer. A replacement source registration brings a new generation and grant key, so unattached relay grants lose their capacity at once and stay as tombstones until expiry, keeping delayed control idempotent.

### Playback

The viewer's game runs the loop in `GameHooks.cpp`, reading a `SpectatorViewerStatus` from `DPlayTransport.cpp` each tick. It may hold the frame, advance once, or run several frames in a slot. The thresholds are all pure functions in `SpectatorPlaybackPolicy.h`, tested in isolation. One record is one frame, about 60.6 per second.

**Wait ladder.** Each wait in this table is bounded, and a bound ending in failure calls `netFailClosed`. A held buffer on a source that still heartbeats has no bound.

| Wait | Constant | Result on expiry |
|---|---|---|
| GameStart, source answered | `kSpectatorGameStartTimeoutMs` (300 s) | `Expired` |
| Source silent after contact | `kSpectatorSourceGoneMs` (45 s) | `SourceGone` |
| Source never answered | `kSpectatorSourceGoneMs` | `NeverAnswered` |
| Gameplay entry rendezvous | `kSpectatorGameplayEntryTimeoutMs` (30 s) | `Reject` |
| Set-seek checkpoint | `kSpectatorSetSeekWaitMs` (15 s) | Watch from record 0 |
| Layout refresh hold | `kSpectatorLayoutHoldDeadlineMs` (10 s) | Fail closed |

`spectatorStartWaitVerdict` decides the first three. A source that heartbeats forever does not arm a timeout, which is why only the 300 s bound applies once contact exists.

**Buffering.** `spectatorPlaybackPrimed` needs `kSpectatorStartupBufferRecords` (120) to start and `kSpectatorRebufferRecords` (90) to resume, plus a cushion bonus. It passes immediately with a terminal, a complete phase, or a source holding nothing more.

`spectatorSourceHoldsNothingMore` is the important exception. The Heartbeat reports the source's frontier, and a source that is alive but paused or at a rendezvous has the same frontier as the viewer for `kSpectatorSourceIdleDwellMs` (2 s). Without this check the cushion could never fill and the viewer would wait forever on a healthy source. For the same reason `spectatorEmptyBufferIsUnderrun` does not count an empty buffer as an underrun when frontiers match. Real underruns raise the bonus by `kSpectatorCushionStepRecords` (120), capped at `kSpectatorCushionMaxBonusRecords` (360).

**Speed.** `spectatorCatchupPlan` enters catch-up above `spectatorCatchupEnter` (target + 300 records) and leaves it at `spectatorLiveTarget`, giving hysteresis. Gameplay speed scales with excess, from 2x to 24x. Outside gameplay catch-up enters as soon as the buffer exceeds the target and always runs at `kSpectatorCatchupMaxPermille` (24x). `spectatorCatchupMustYield` stops a multi-frame slot at any scene or phase boundary. Below target, `spectatorElasticPlan` slows smoothly (97%, 90%, 75% under 60, 30 and 15 records), hiding jitter before a full rebuffer. A viewer's skip request latches `g_spectatorFastForwardLatched`, forcing maximum speed until the buffer is at target; it does not seek.

**Verification.** Before running a frame the viewer compares its own pre-state hash to `record.stateHash`. Outside gameplay a mismatch fails at once. At gameplay entry, `spectatorPreStateDecision` returns `RetryEntry` while the local segment is not yet active and the deadline has not passed, because the two machines reach this point at different wall-clock times. A confirmed mismatch fails with `kNetConfirmedDesync`. `commitPlaybackRecord` checks the hashes again and rejects anything off the chain. `spectatorSourceStopsOnDispatchReturn` is the source-side counterpart at the end: a zero dispatch result in the menu or character-select scene quits the process, so that is where the stream seals, whereas zero in a match only returns to character select.

**Why the numbers look like this.** A viewer is biased towards latency: a few seconds behind costs an observer nothing, and every hitch costs the picture. Two seconds of cushion covers a lost packet and its repair several times over. A route that has starved a viewer once is expected to do it again, so each underrun episode raises priming, rebuffering and the catch-up band together, and a bigger cushion is cheaper than a second freeze. The elastic watermarks stay fixed because they are about running dry. Elastic slowdown repeats the last presented frame at evenly spaced slots and never invents or skips a canonical record. Resuming at 90 records rather than the full prime shortens a visibly frozen frame, and the elastic cadence rebuilds the rest.

**The source frontier must match exactly.** `spectatorSourceHoldsNothingMore` measures the dwell on the source's own frontier from the Heartbeat, not on the receive rate, because a slow route is still delivering records worth waiting for. The match must be exact, not "at or past". Records sent after a Heartbeat often arrive before the next one, so on a live source the viewer is regularly just past the last frontier it heard. If Heartbeats are what is being lost, that stale reading stays still for the whole dwell while records pour in. Being past the frontier means the reading is old, not that nothing lies behind it. A terminal also releases priming for a separate reason: a sealed archive never grows to the threshold, and completion needs playback to apply and verify the tail, which priming would block.

**Catch-up has two ceilings.** The speed ladder is an upper bound. In practice the wall-clock budget from `spectatorCatchupSliceMicros` limits convergence, so a slow machine loses catch-up speed and keeps its cadence. `kSpectatorCatchupSliceMicros` (24 ms) buys about ten gameplay ticks per slot, overruns the slot and presents at about 40 Hz while catching up, which is the intended trade for a viewer who wants to reach live. Outside gameplay a tick is far cheaper and nothing on screen needs frame accuracy, so `kSpectatorCatchupMenuSliceMicros` is larger and the slice is the only real limit. `spectatorEstimatedSlotsToLive` feeds the on-screen ETA only and is never a control input.

**The layout hold is local work.** `kSpectatorLayoutHoldDeadlineMs` exists because nothing the source does can end that wait. A player would notice the same hang through peer timeouts, and a viewer has no peer. The set-seek wait, by contrast, must outlast the source's whole re-push sequence, since a pushed checkpoint has no repair message.

### Title-bar states

`updateWindowTitle` in `DPlayTransport.cpp` builds a title holding the player names, the state below, the game id, the played and received record counts, and the buffer size.

| State | Condition |
|---|---|
| CONNECTING | Not streaming, no GameStart |
| BUFFERING | Not streaming, has GameStart |
| JOINING | `SpectatorWait::Joining`, booting towards the first record |
| BUFFERING n/m | `SpectatorWait::Filling`, not primed or empty |
| BUFFERING - needs a set event (n held) | `SpectatorWait::AwaitingEvent`; records are held behind a missing event |
| WAITING FOR THE MATCH (n held) | `SpectatorWait::SourceIdle`; the source has nothing more yet |
| LIVE | Primed, buffer not empty, not catching up |
| CATCHING UP (n excess, ~s) | Catch-up active |
| PAUSED | The viewer's own local pause (`g_spectatorLocalPaused`), not a pause on the host |
| SEALED (n left) | Terminal known, playing out the backlog |
| COMPLETE, FAILED, REJECTED | `ViewerPhase::Complete`, `Failed`, `Rejected` |

SEALED takes priority over PAUSED, and PAUSED over the wait states.

### Set-seek checkpoints

A checkpoint lets a late viewer start at the current match instead of walking from record 0.

**Capture.** `spectatorSetCheckpointTick` arms once per match, 30 ticks after the match opens and only when the stage value is valid. `spectatorSetCheckpointCaptureAtPublish` then copies the rollback slot for the frame being published, so the state is canonical, not whatever the possibly rolled-back live frame holds. It is sent before `publishConfirmed` for that frame, so a checkpoint's record cursor is exactly the next record to run. Three regions go in:

| Region id | Content |
|---|---|
| 1 | Flat rollback snapshot, schema `'QRB1'`, must equal `g_rb.totalSize()` when restored |
| 2 | 40-byte `'QSSB'` boot descriptor: roster, stage, match ordinal and timeline seed (`packSetSeekBoot`) |
| 3 | Canonical virtual audio state |

The checkpoint carries `digestChainPrefix` and an `eventCursor`, placing it on the rolling chain, and a whole-state hash.

**Storage.** `applyCheckpoint` keeps up to `maxCheckpoints` within `maxCheckpointBytes`, and only the newest blob is offered. If the archive has not reached the cursor (`ArchiveResult::Gap`), the reference waits in `_pendingCheckpointRef`.

**Delivery.** A viewer sets `kHelloFlagWantsNewestCheckpoint` in its Hello. The source pushes chunks rather than serving requests, so one lost chunk would otherwise be fatal. `kCheckpointPushPasses` (6) whole passes, the first plus five re-pushes, spaced `kCheckpointPushRepassMs` (500 ms) apart repair that. `SpectatorViewer` collects the chunks in its own buffer (`CheckpointReassembler` is used only by tests); `decodeCheckpoint` validates length, CRC, region hashes and digest, and `checkpointMatchesCompatibilityIdentity` confirms the build.

**Restore,** driven by `SpectatorSkipState` in `GameHooks.cpp`:

1. `Waiting`: take the checkpoint. A refusal here, or 15 s with nothing, falls back to watching from the start. No harm is done yet.
2. `Booting`: the direct boot that `startReplayDirectBootToMatch` started on accepting the checkpoint runs the region 2 roster into the match.
3. `Restoring`: wait for the rollback layout, load with `g_rb.loadFromForeign`, restore audio, then compare `netLiveTaggedGameplayHash` to the checkpoint's `stateHash`. By this stage the game has been rebooted, so a mismatch fails closed instead of falling back.
4. `spectatorViewerResumeAtCheckpoint` calls `SpectatorViewer::resumeAtCheckpoint`, which rebases record and event cursors and the digest, binds the set timeline, and sends a resume Hello.

Only the `--latest` option on the `spectate` verb asks for this. The interactive spectate path leaves `g_spectateWantsLatest` false: a menu viewer starts at record 0 and uses catch-up. Local seek commands such as `NextMatch` and `SeekRound` are logged as unavailable in spectating, and `SeekMatch` only stores a request.

**Size limit.** `publishCheckpoint` refuses anything over `kMaxSidecarPayloadBytes` less 8 bytes (about 2 MiB), even though its slots are larger. A refusal is not a fault and leaves the checkpoint armed for retry, so a larger state gets no checkpoint and a late viewer falls back.

**Request and diagnostics.** The caster's preflight and the game's own Hello share one identity and generation, and the preflight carries no flags. The want-checkpoint flag therefore arrives on a later Hello of the same generation, and that handling ORs it in so a flagless retransmission cannot clear it. That later Hello can also be dropped by the replay window, so the sidecar decodes the flag itself and forwards it with `setViewerWantsNewestCheckpoint`. A new generation at the restored cursor clears the offer, which is also the only signal the source gets that the checkpoint arrived.

The rebind is held to the same standard as any resume: the claimed digest must equal `chainDigestAt` for that record and event, so a tampered checkpoint is refused at admission. When a push does not progress, read `checkpointPushBlocked` in the sidecar's viewer diagnostics:

| Value | Meaning |
|---|---|
| 0 | No obstacle |
| 1 | No checkpoint reference |
| 2 | No blob |
| 3 | Blob id differs from the reference |
| 4, 5 | Offer could not be encoded or appended |
| 6 | All passes spent |
| 7 | Waiting out the gap between passes |
| 8 | Packet budget ran out before this viewer |

### Resilient topology

`ResilientTopologyAuthority.*`, `ResilientTopologyClient*`, `ResilientSpectatorRuntime.*`, `ResilientSpectatorNodeRuntime.*` and `src/spectator/ResilientSpectatorDistribution.*` let a viewer relay the stream to other viewers, up to `maxActiveDepth` (3), so the host's upload is not the only source.

- `TopologyAuthority` runs inside the host's sidecar behind `TopologyQsrControl`, a control socket distinct from the QSP port, and `onDatagram` rejects topology magic arriving on QSP. It is the only party that can enroll a node, assign a parent or issue a lease. Its messages are signed with an ECDSA P-256 key (`TopologyAuthoritySignature.*`) and enrollment is checked against proven endpoint provenance (`EndpointProvenance`).
- `ReleasedTopologyClientRuntime` runs in the viewer's game process, armed after the first `InitAck` (`armReleasedTopologyClientAfterInitAck`). It enrolls over a control link authenticated from the viewer's own grant (`controlv2::validateProof` on the host).
- `ResilientSpectatorNodeRuntime` is the relaying viewer's data plane. In the shipped path (`NodeRuntimeIngressMode::CanonicalHostAttested`) it builds no upstream viewer and takes records through `QtaVerifiedCanonicalIngress`. An upstream viewer with `relayIngestOnly`, which verifies and caches records without playing them, exists only in the default `SignedQsp` mode, which no shipped path selects.
- A viewer changes parent with `armValidatedParentRouteTransition`, which accepts the new route's datagrams only at a digest-matching cursor.

**Punched routes.** Topology bootstrap does not start on a punched route: `armReleasedTopologyClientAfterInitAck` logs that punched QSR is not runtime-proven and stops there. Only direct and relay viewers can enroll, even with `--spectator-topology`.

**Vocabulary.** The code calls the ordinary viewer service E0. QSR is the topology control datagram protocol on its own socket, and QTA is the credential-authenticated envelope around each QSR payload. S1 is the relay and S2 the child in a paired move. Ordinary viewers never receive a topology `NodeId` or credential, and every failure here clears topology material only and leaves E0 playback alone. Once attached, malformed or stale control packets are ignored instead of causing a teardown, so a spoofed datagram cannot disturb a viewer. The client's local liveness deadline is the real cleanup path, and it is checked before draining QSR so a queued heartbeat cannot revive an expired attachment.

**A MAC proves possession, not origin.** QTA is bidirectional for an enrolled leaf, so its MAC only shows the sender holds that credential. Assignments and activations also carry a separate host ECDSA signature (`TopologyAuthoritySigner`) whose public key was in the authenticated enrollment transcript. An outer MAC also does not make the inner message eligible for anything: `decodeControl` decodes and authorizes the complete inner message before it binds the QSR source or consumes an inbound sequence. The first valid NodeAdvertisement sets the QSR endpoint from the packet's real source, since a port-0 client cannot know how the host sees it, and that endpoint is then pinned. Only the route is normalized this way; every other field must still pass the admission check.

**Endpoints do not vouch for each other.** QSP, QSR and relay-data each name a separate real UDP endpoint in one shared namespace, and proof of one is not proof of another: a punched QSP mapping says nothing about QSR. Topology magic on the QSP socket is consumed even when malformed, so that socket cannot become a second topology ingress. Route provenance, physical ingress and the signed relayed-QSR offer vary together. A relayed viewer speaks only through the relay lane and declares the `TrafficRelay` shape, a direct or punched one can never arrive relayed, and any mismatch is refused before a response exists to sign. The virtual endpoint under `kRelayVirtualIpv4BE` is a routing identity, not a credential, and is never dialled: a send to it rides the authority's relay lane and fails closed without a live lane. That lane is held open by a dedicated TCP connection whose lifetime is its liveness signal. Losing the lane stops new relay-ingress enrollments on the next service turn.

**Clocks do not cross machines.** Host monotonic values are never compared to a local clock. The signed lease duration is converted once into `_credentialDeadlineMs` when Info arrives. An activation deadline and `expiryMs` are both host-clock facts, so only their nonnegative difference is portable, and it is subtracted from the local credential deadline.

**Cursors and capacity are exact.** Each enrollment generation binds the frontier frozen at enrollment, so a delayed offer or receipt can still be rebuilt by the child, and root issuance separately proves that the cache still retains that boundary. Readiness applies only to the frontier it named, so a later archive advance needs a fresh RelayReady. A parent receipt must equal the viewer's current verified QSP frontier. The root cache is fed only after `appendConfirmed` accepts a record, so speculative or rejected publications never enter recovery state, and a process start time never counts as progress (`noteVerifiedProgress`). A signed lease states the relay's remaining capacity at that frontier, counting host reservations for children that have not yet said Hello, so a literal 1 would contradict the multi-child policy.

**Replay and retransmission.** Retransmitting is normal, so a repeat must reproduce the same material and never mint a new nonce, cursor, route or deadline:

- A repeated proof for the same live QSP binding returns the same child material; any changed generation or endpoint stays closed until the rebind path runs.
- A retried NodeAdvertisement gets the retained lease back. A retried redemption gets the same ParentCommit and does not reattach. An attached child with no sendable receipt is rolled back on both owners, and the nonce stays spent.
- The client admits an authenticated lower sequence only for that exact receipt, because a liveness envelope can overtake the ParentCommit it follows.
- In `QtaAuthorityContext`, the nonce is reserved before the runtime's local checks and committed only after a child is fully installed, so a failed install does not burn a host-issued nonce. The ledger is owned outside `ResilientSpectatorNodeRuntime` and survives its stop and start.
- `_retiredNodeIds` keeps node ids from ever being reused, and counts against `maxTopologyNodes`, so a churny peer fails closed instead of growing bookkeeping over a long session. `reserveTopologyControlSequence` makes the envelope counter terminal at its maximum instead of wrapping, since a wrap would make a very old packet look fresh.

**A paired move has two signed phases and one data-plane gate.** `issueRelayChildActivation` reserves a transaction delivered to both participants over their bound routes. A partial UDP send retries that exact transaction. Each participant answers with its role's receipt, and only after both does the host mint a second, independently signed commit. A receipt is never a data-plane permission. `markRelayChildActivationCommitSent` records local UDP sends only, because QSR has no commit receipt, so the host's S1-to-S2 adjacency is not proof of delivery. S2 attaches only after DPlay's old-source-preserving gate accepts a real candidate datagram (`armValidatedParentRouteTransition`, then `completeParentRouteActivationAfterIngress`). A commit is never redirected: if the route was replaced, the receipt path fails closed and a retirement follows.

**Retirement names one exact binding.** A retirement matches the complete immutable tuple and is delivered only to the exact QSR route recorded at issue, using a credential snapshot taken while it was valid, so it still reaches the prior endpoint after a rebind and never falls back to QSP. Node id alone never selects a child: `QtaVerifiedChildRetirement` is the only capability, and there is no generic delete. Expiry queues retirements before clearing the record, because the record's binding is what reaches the recipient, and if the bounded queue is full the reservation stays and the next service tick retries. Only `serviceExpiry` may erase an attached record, since it can also release generic-root capacity. The client keeps the withdrawn tuple so a delayed activation cannot revive a candidate after its retirement.

**Socket ownership.** The relay-data socket passes once from DPlay into the client runtime and is never rebound by number. DPlay holds only a `ReleasedTopologyRelaySocketBorrow` and must compare both socket and generation before reusing a WSA event, because Windows can recycle a closed socket's number. Control reconnects rotate the request nonce so a delayed response from the abandoned link cannot advance the new exchange, and they never extend the original setup deadline.

**Authority boundaries.** QSR control and QSP data are separate protocols. `hasResilientControlMagic` is deliberately broader than a successful decode, so a malformed control packet on the QSP socket is consumed as topology traffic and is not parsed as QSP. QTA envelopes wrap every QSR datagram between a node and the authority, and a relay carries them opaquely: `TopologyRelayAdapter` checks only the outer shape, and the MAC stays end-to-end.

Two kinds of key cover different threats. A QTA credential is symmetric and shared with the node, so it proves delivery but cannot authorize anything the host alone decides. Those decisions carry the host's ECDSA signature, which no node can produce.

**Paired parent changes.** Moving a child onto a relay takes two signed steps. The host sends one `RelayChildActivation` to both the relay and the child, and each returns a `RelayChildActivationReceipt`. Only when both receipts have matched the reservation does the authority create and queue the `RelayChildActivationCommit`. The commit embeds the whole activation, including its signature, and is signed again, so a valid activation cannot be retargeted into a new commit. Receiving an activation changes no data-plane ownership. Retirements and leases carry complete binding tuples so a stale message cannot act on a replacement that reuses an endpoint or node id.

**During the switch.** The old parent stays active and keeps its own traffic. Candidate packets are parsed against a separate replay window (`candidateInbound`) so a bad candidate cannot poison or fail the working stream, and only `commitValidatedParentRouteIngress` moves that window into place.

**Continuity.** A `ContinuityProof` echoes the requester's `parentGeneration`, so a proof from one recovery race cannot satisfy a later one. It may also name a checkpoint. A named checkpoint inside the node's own verified window must match its local digest, and a mismatch is treated as a lying provider, like a wrong overlap digest. One outside the window cannot be checked and stays advisory.

**Cache bounds.** `ResilientLimits::valid` requires `maxCachedRecords` to cover the sum of the race, continuity, retransmission and bootstrap allowances (`requiredCacheRecords`), and requires the record and event maxima to fit jointly within `maxCachedBytes`. Checkpoints use only what is left of that budget. A set event arrives before the record at its boundary, so a full cache must make room early: `evictOldestRecordBoundary` retires one complete oldest boundary before admitting the event, otherwise a valid stream would hit the memory limit between its event and record. Children the eviction overtakes are marked for controlled resync (`markLaggingChildrenForResync`), so a slow child cannot pin memory or block another.

**Relay lanes.** `SpectatorRelayAdapter.cpp` checks the viewer id inside each QSP payload against the viewer id bound to the relay lease before it wraps or unwraps the datagram, so one lease cannot act for another viewer. `RelaySourceAdapter` gives each relayed viewer a distinct virtual endpoint, which keeps the source's endpoint-keyed state from treating all of them as the relay's one address. Topology grants start with their own domain magic (`kTopologyGrantMagic`), so one can never verify as a spectator grant.

### Shipped flow versus test-only

| Component | Status | How to confirm |
|---|---|---|
| Host sidecar, QSP, archive, viewer, playback, route, grants | **Active** in every served match | `SPECTATOR_SERVICE_SIDECAR` set in the host path of `Main.cpp` |
| Set-seek checkpoints: capture and storage | **Active**, one per match | `spectatorSetCheckpointTick` |
| Set-seek restore | **CLI only** (`--latest`) | Test `qoh99_game_e2e_spectator_set_seek_latest` |
| Forced routes (`--spectator-route` in the test harness) | Test and diagnostic | `requestedRoute` is zero in ordinary use |
| Topology authority on the host | **Runs** when `sidecarConfigFor` sets `topologyActivation.requested` (a non-relay candidate on the serving port, preferring the public one) | `topologyAuthorityReady` |
| Topology activation | **Manual, off by default**: the viewer needs `--spectator-topology` (no menu path passes it), and the host presses T and picks a relay and a child in `selectTopologyRelayChild`. Nothing promotes automatically. | Test `qoh99_game_e2e_released_topology_direct` |
| Join-side collector (`ClientPrivateSidecarLaunch`, `planClientPrivatePublisher`) | **Off by default**: requires `clientPrivateSpectatorPublisher` in the config file, with no settings row. The host always sends a `ClientRootOffer` through `hostMintAndSendClientRootOffer`; a joiner with the setting off ignores it. | |
| Client root activation (`sidecarIssueClientPlayerRootActivationForViewer`) | **Test only**: no production caller, so an armed root stays dormant | Test `qoh99_released_client_root_control` |
| Connected V4 stream (`SpectatorConnectedArchiveV4`, `SpectatorConnectedSourceSessionV4`, `ConnectedReplayPublisherV4`) | **No game data in shipped use**: `publishStart` still sends the descriptor, so the sidecar binds an empty connected archive, and `publishSeal` queues a set seal, but nothing outside `src/game/spectator/` calls `connectedReplayPublisherV4` or `publishConnectedOutOfGameV4` | `src/spectator/tests/test_spectator_v4.cpp` |
| In-process source (`SPECTATOR_SERVICE_INPROCESS`) | **Not launched**: no caster path sets it with `serveSpectators` nonzero. The branch and `g_netSpectatorArchiveBegun` remain in the game. | `test_production_phase.cpp` |
| `PublicationKind::Pause`, `Resume` | **Test only**: `decodePublicationFrame` never produces them, so `SidecarPhase::Paused` is not reached from the pipe. A host pause appears to viewers as a static frontier, hence WAITING FOR THE MATCH. | `test_spectator_sidecar.cpp` |
| `verifyPeerPrefix`, `acceptPeerPrefix` | **Test only** | `test_spectator_sidecar.cpp` |

When changing behaviour, start from this table. The largest files in `src/client/spectator/` are mostly the opt-in topology paths, and a bug report from an ordinary match will almost always land in `SpectatorSidecar.cpp`, `SpectatorSession.cpp` or the playback code in `GameHooks.cpp`.

## Replay pipeline

A replay is a recorded input timeline plus proof of what those inputs produced. Playback does not restore a saved match. It boots the real game, feeds it the recorded actions and compares state hashes on every frame. Nearly every rule below exists to keep that comparison meaningful.

| Directory | Runs in | Owns |
|---|---|---|
| `src/replay/` | Both processes (no Win32 game state) | Types, codec, timeline validator, digests, recording coordinator, publisher queue, playback session, seek cache, takeover controller, wire format |
| `src/game/replay/` | Game process (`qoh99_netplay.dll`) | `ReplayRuntime` (recording and playback state), `ReplayIpcWorker` (pipe thread) |
| `src/client/replay/` | Caster | Recorder host, writer, parser, preflight, broker, catalog, browser model, annotations, battle log |
| `src/GameHooks.cpp` | Game process | The native seams: when to capture, restore, tick or hold |

Each `.q99r` file holds exactly one game. A set is a folder of files tied together by a series UUID.

Two file formats share the extension and the magic number. `hasV2Prefix` tells them apart when the catalog scans. The V2 format (types and files with a `V2` suffix) is the only one recorded or played. The multi-match set format (`ReplayWriter`, `ReplayParser`, `ReplayRecorderHost`, `ReplayBroker`, `ReplayRecovery`) can still be listed by the browser, but it cannot enter playback: `loadReplayPreflightInputV2` refuses anything that does not parse as one V2 game.

### Recording: data flow

```
GameHooks native frame
  -> replayRecordConfirmedV2           (game thread)
  -> GameEpochTrackerV2                (is this the start of a new game?)
  -> ReplayRecordingCoordinatorV2      (validate, stamp digests, build ticks)
  -> ReplayPublisherQueueV2            (lock-free SPSC ring)
  -> ReplayIpcWorker                   (pipe thread)
  == wire v3 named pipe ==
  -> ReplayRecorderHostV3              (caster worker thread)
  -> ReplayWriterV2                    (bytes staged in memory)
  -> <root>\<source>\<set>\<game>.q99r, then battles.q99log
```

**What gets recorded** is decided before launch by `planRecording` in `src/client/Main.cpp`. Netplay, local VS and spectator sessions each have their own setting. Replay playback, Training, VS CPU, probe and self-test launches are never recorded. If the local compatibility identity cannot be qualified or the replay folder cannot be created, `startRecorderForLaunch` logs that the match is not being recorded and the match proceeds. A recorder failure must never affect play.

**Only confirmed frames are recorded.** Rollback netplay simulates speculative frames, but the recorder only sees frames that rollback cannot change. Its name for this is `ProvenGameFrameV2`.

**Game boundaries** come from two steps. `replayRecordStageFreshGameV2` stages a launch barrier in `GameEpochTrackerV2`. `observeBeforeFrame` then arms the game when the lifecycle actually enters the initial match, and hands `armFreshGame` a `FreshGameDescriptorV2`.

**`ReplayRecordingCoordinatorV2`** is a state machine: `WaitingForFreshGame`, `WaitingForFrameZero`, `Recording`, `AwaitFirstWinScreenTick`, then back to waiting. `Failed` is the error state.

- It refuses any topology other than two recorded humans on the identity route (`topologyQualified`).
- Frame zero is not the first frame seen. The coordinator first sees the round's setup tick (`isInitialSetup`) and expects frame zero on exactly the next source frame.
- Frame zero is published as `BeginGame`, carrying the `ConfirmedGameStartV2` and a tick with `GameStarted` and `RoundStarted` as pre-events.
- `acceptRecording` requires source frames to be contiguous and round transitions to be legal. Each round's `RoundEnded` and the next `RoundStarted` are derived from `RoundTransitionProofV2`, not inferred from hashes.
- The game ends on the first win-screen tick. `acceptFirstWinScreen` checks it against the latch taken at the final transition (`matchesFirstWinScreenTick`) and attaches `GameResult`.

A fault before any file exists goes through `failWithoutFile`. A fault afterward goes through `failCurrent`, which publishes an abnormal end with `TerminalReason::RecordingFailure` (`RecorderOverflow` for a publisher overflow) so the validated prefix survives as a sealed file.

**Every candidate is validated before it is committed.** The coordinator validates against copies of `validator_` and `canonical_` and assigns them only after the queue accepted the record. The writer does the same. A rejected tick leaves no trace in the running digests.

**`ConfirmedTimelineValidatorV2`** is the single definition of a legal timeline. The coordinator, writer, parser and playback buffer all run it, so the rules cannot drift between writing and reading. `ConfirmedTimelineFaultV2` is the list of what it rejects: frame and source gaps, event sequence gaps, lifecycle and state-proof discontinuity, rolling digest mismatch, and a terminal that does not match the last frame.

**The queue always has room for the end of a game.** `ReplayPublisherQueueV2` caps ordinary records at `capacity - 1`, so one slot stays free for `publishEndGameReserved`. An ordinary overflow latches `ordinaryOverflowed_` and rejects everything ordinary afterward, because a hole in the timeline can never be repaired. The game thread never blocks on it. `beginGeneration` is only legal when producer and consumer are both stopped.

Commit acknowledgements flow back through `CasterGameCommitted` to `onCasterCommitAck`. At most `kMaxPendingReplayCommitsV2` games may be awaiting one; beyond that the coordinator faults with `CommitBacklog`.

**What records, and how a fault ends it.** Free Play launches as `MODE_OBSERVE` with a 1v1 row, so `planRecording` treats it as local VS under `recordLocalVs`. A host-side fault during recording loses the game: `failProtocol` calls `writer_->abandon()` and resets, so any cursor, validation or writer error discards the staged bytes with no sealed file. Only the disconnect path seals a partial game.

**The game thread never does I/O for a recording.** Everything it calls is a bounded copy into a preallocated ring, and all pipe work runs on the `ReplayIpcWorker` thread. A recorder that cannot keep up disables itself and the match continues unchanged. When recording is disabled the worker still drains what was already published, because those records are real confirmed history, and emits the terminal only once the queue is empty.

**A launch barrier is what makes a new game.** In the native game a later round passes through the same `matchSub` 5, -1, 0 sequence as a match start, so that sequence alone cannot define a game; only a staged barrier followed by the initial setup can. The barrier is captured before the game assigns and loads the stage in that setup tick, so `isValidStagedFreshGameStartV2` accepts an unverified all-zero stage that the ordinary manifest validator rejects. That staging form is never written.

**Abandoning has two halves.** `replayRecordAbnormalEndV2` reaches only the coordinator. A game can also be staged and not yet armed, and a player leaving a match with F1 lands in that window for the one frame the game spends at `matchSub` -1. If the staged barrier survived, the next record still in setup state would arm the dead game and record character select under its roster, and the tracker would refuse every later game. `replayAbandonOpenGameV2` therefore resets the tracker as well as ending the coordinator's game.

**Spectated names arrive late.** A viewer's recorder lanes start with the viewer's own nickname and a placeholder. `replayRecordSetSourceLaneNamesV2` installs the real players' names and is refused once a game is staged or in progress, because a set whose games disagree about who played is worse than one filed under a placeholder.

**Setup tick and frame zero use different hash domains.** The setup tick runs before gameplay entry has resolved the rollback layout, so it can be hashed through `netFrontendStateFingerprint`. Frame zero's pre-state uses the stronger gameplay, FPU and audio profile. Comparing the two byte for byte would be meaningless, so the coordinator links them with `expectedFrameZeroSource` and `expectedFrameZeroBefore` (exact source adjacency plus lifecycle equality), and frame zero records its own pre-state hash.

**Stale publishers cannot harm a newer game.** `tryPublishOrdinaryFor` and `publishEndGameReservedFor` carry the generation the caller captured. A mismatch only increments `staleRejects_`. It does not latch `ordinaryOverflowed_` or `disabled_`, because those records were never part of the current recording.

**Palettes are cosmetic, but they share the ordinary path.** `publishPalette` returns false outside `ReplayGameRecordStateV2::Recording`: before frame zero there is no game to attach the chunk to, and after the end the game is sealed. A palette is in no hash or proof, and a false return means the game is recorded in the character's own colours. It does go through `tryPublishOrdinary`, however, so a palette that meets a full ring latches `ordinaryOverflowed_` exactly as a tick would, and the ticks after it are rejected.

### Recorder host and writer

`ReplayRecorderHostV3::handleFreshFrame` is the state machine on the caster side.

- `RecorderHello` must match the game PID and `runtimeCompatibilityId`.
- `RecorderFrames` and `RecorderEvents` must start at exactly `expectedFrame_` and `expectedEventSequence_`. They arrive separately, so `pendingTick_` reassembles them into `CanonicalTickBundleV2` values before they reach the writer.
- Protocol, cursor and validation errors go through `failProtocol`. A bad palette only increments a counter, because a palette is cosmetic.

**Nothing reaches disk until the game ends.** `ReplayWriterV2` appends every chunk to `stagedBytes_`. `maybeFlush` only maintains counters. `finalize` creates the final name with `createNew`, writes the whole buffer in one go, flushes and closes. Consequences to keep in mind:

- A `.q99r` on disk is always a complete file unless the disk or a later copy damaged it. The writer creates no `.part` file, and `partPath()` stays empty.
- A name collision retries with `_2`, `_3` and so on. Disk-full and access-denied errors fail immediately.
- If the game process dies, the pipe closes and `sealOpenGameOnDisconnect` seals what was received, with `TerminalReason::ProcessExit`, or `RecordingFailure` if the process is still alive. `makeAbnormalEnd` computes a valid terminal from the validated tail. The file stays playable because the terminal is honest about how it ended.
- If the caster itself dies mid-game, that game is lost.

`ReplayParserV2` still classifies `.q99r.part` names and unsealed tails, and `ReplayRecoveryV2` can reseal a validated prefix under a new UUID derived from its content (`deriveRecoveredUuid`). Recovery has no caller outside its tests, and the scanner accepts only `.q99r`.

**Pipe access is negotiated.** The broker pipe is duplex, but the recorder and spectator publication servers are `PIPE_ACCESS_INBOUND`, and Windows fails a client asking for `GENERIC_READ` on those with `ERROR_ACCESS_DENIED`. The worker's connect tries duplex and falls back to `GENERIC_WRITE`; requesting read and write unconditionally would silently disable every recording. Recorder attempts are spaced by `reconnectBackoffMs` because connect fails instantly while the server does not exist yet, and an unspaced budget would burn out in milliseconds. The playback broker's pipe exists before the game is asked to play, so a spent attempt budget there really is a dead channel.

**Every frame is bound in its envelope.** `src/replay/ReplayIpcWire.h` is the single definition of the framing on both ends. Generation, nonce and wire version sit in the header of every frame, so a worker from an earlier launch cannot feed a new one even if it skips the handshake, and a version mismatch is refused on the first frame. The payload CRC is a header field covered by the header CRC, so a verified header authenticates the length and the payload digest before any payload byte is read. All of it is treated as untrusted even though the peer is local.

### The `.q99r` V2 container

```
Prefix (kPrefixBytes)         magic, container version, feature flags, game UUID, CRC
Chunk: GameManifest           identity, simulation contract, roster, stage, display metadata
Chunk: CompatibilityIdentity  complete content identity (when attested)
Chunk: GameStart              deterministic start: RNG, FPU, configuration, display state
Chunks: EventBlock / InputBlock / Palette, in timeline order
Chunk: GameEnd                terminal reason, tallies, final digests
Chunk: FooterIndex            offsets, counts, round index, file digest
```

Every chunk has a `ChunkHeaderV2` (`kChunkHeaderBytes`): kind, a contiguous `sequence`, the record range it covers, and CRC32C for both payload and header. `writeTick` flushes pending frames before writing events, so event blocks split input blocks at the exact frame they belong to. The parser's `TickAssembler` relies on that to rebuild ticks. An input block holds at most `kMaxRecordsPerInputBlock` frames.

`ChunkKindV2` also defines `ProofBlock` and `Checkpoint`, and `isCriticalChunkKindV2` marks them and `Palette` non-critical. The writer emits neither of those two. Seek snapshots stay in memory (see below). `src/replay/ReplayCheckpoint.h` defines a checkpoint encoding that nothing outside its tests uses, and its rule still stands: no pointer-bearing savestate buffer goes into a file.

A frame (`ConfirmedGameFrameCoreV2`) stores the actions per lane, the lifecycle before and after, the native dispatcher's return value, and pre/post state hashes. It stores no game state.

Layers of integrity, each answering a different question:

| Check | Covers | Answers |
|---|---|---|
| CRC32C per chunk | One header or payload | Was this chunk torn or corrupted? |
| Rolling digest | Each frame and event, chained (`advanceReplayDigestV2`) | Is the timeline in order with nothing removed? |
| `CanonicalGameDigestV2` (SHA-256) | Canonical manifest, start, canonical events, frames, terminal | Is this the same game, regardless of who saved it? |
| File digest (SHA-256) | All bytes before the footer, then a projection of the footer | Is this exact file intact? |
| `validatedSourceRangeDigest` | The validated byte range | Is what the broker sent the range the parser proved? |

**Keep the canonical digest free of display data.** `feedCanonicalManifestV2` does not feed `DisplayMetadataV2`, and the writer never feeds palettes to it. That is what lets the host rewrite `recordedUnixSeconds` after the coordinator has already computed the digest, and lets a player and a spectator produce the same canonical digest for one game. `ReplayWriterV2::finalize` recomputes the digest and refuses to seal if it differs from the one the coordinator sent.

The file digest has a self-reference problem, since it is stored in the footer it covers. `writeFooter` hashes the footer header with its last 8 bytes (the two CRCs) zeroed, plus `encodeFooterIndexFileDigestProjectionV2`. `ReplayParserV2::parse` must reproduce that sequence exactly.

**Nothing hashes object memory.** The digests consume explicit little-endian scalar encodings, so padding, layout and compiler cannot change them. The rolling digest is a corruption and ordering check, not an authenticity claim.

**Palettes are cosmetic and still checked.** A palette chunk is in no hash and no proof, so a malformed one is dropped and the game replays in the characters' own colours. Native fact: entry 0 of every palette variant is the game's transparent key, and the game's loader forces it to black. `decodePaletteBlockV2` rejects a table that disagrees, since it would draw the fighter's cut-out opaque.

**Character identity.** Native facts: the selector grid is 3 rows by 8 columns, the eighth column is visually hidden, and IDs 34 to 36 are internal partner and form actors (Kaede's Edifel, Mizuho's partner Kanako, the C-Serika form). They are never roster participants, and selectable Kanako 24 is a different object from partner Kanako 35. The resolved native ID is the identity; cursor cell and alternate flag are only a proof that `verifySelectorProof` can check. `catalogId` hashes the catalog's content and goes into the simulation contract, so editing an ID, label, flag or the entry order makes existing files fail closed instead of silently renaming a fighter. Aliases are not hashed, because they cannot change identity. Spellings in the table follow the executable's own tokens, not player guides.

**The structs are decoded models, not file images.** No C++ object is serialized. Every integer has an explicit little-endian encoding in the codec, so struct layout and padding cannot leak into the format. Counts are checked against product constants such as `kMaxRecordsPerInputBlock`, never trusted from the file.

**Actions are stored in the compact domain.** `kActionMask` allows bits 0 to 3 for A, B, C, D and bits 8 to 11 for Up, Down, Left, Right, and any other bit is a hard decode failure. The game's own reader uses the expanded `kLogicalFightMask`, with directions at bits 16 to 19. Everything in a file, on a wire or in a frame is compact. Anything handed to the game goes through `logicalFromCompact`, and anything captured from it through `compactFromLogical`. The two domains share the button nibble, so a missing conversion keeps every button working and silently loses every direction.

**The critical flag does not make a new chunk kind compatible.** `decodeChunkHeaderV2`'s checks reject a kind outside `isKnownChunkKindV2` before the flag matters, and they also require the stored flag to equal `isCriticalChunkKindV2`. `Palette` is non-critical because that describes it truthfully; an existing reader still refuses a file containing a kind it has never heard of.

**Stage identity is catalogued, not guessed.** In the native code, a logical stage index (0 to 27) is mapped through a 28-entry table to the applied background resource ID (1 to 28) during the match setup tick, and that ID selects the stage files to load. In the supported executables the table is exactly 1 to 28. A replay stores the applied ID plus a `catalogEntryId` from `StageCatalogV2`, which `validateIdentity` checks. It never derives an index from a path or accepts a bare integer. Catalog IDs are FNV-1a over a fixed little-endian byte stream: they are compatibility identities, not security claims.

### File metadata and names

`DisplayMetadataV2` carries what the browser shows: `recordedUnixSeconds`, `timezoneOffsetMinutes`, `sourceKind`, per-player delay, rollback window, RTT, jitter, loss, title, route label, `seriesUuidLow`/`seriesUuidHigh`, `gameOrdinalInSeries` and `previousGameDigest`. `DisplayMetadataFlagV2` says which groups are present, and the codec rejects values for an absent group.

`gameOrdinalInSeries` is zero-based. `gameGeneration` and the battle log's `gameNo` are one-based.

Files land at:

```
<replay root>\<source>\<MM-DD_a-vs-b_XXXXXXXX>\<YYYY-MM-DD_HH-MM-SS_hint_XXXXXXXX>.q99r
```

| Part | Built by | Rule |
|---|---|---|
| `<source>` | `sourceFolderNameFor` | `netplay`, `offline`, `spectate`, `recovered`. Never translated: changing one splits an existing library. |
| Set folder | `setFolderNameFor` | Date from the manifest's own clock and timezone. Both nicknames if both exist, otherwise character names. Suffix is the top 32 bits of `seriesUuidHigh`. |
| Game file | `ReplayWriter::buildBaseName` | Local time, hint, then the top 32 bits of `gameUuidHigh`. |
| Hint | `gameNameHintFor` | `<netplay, spectator or local>_G02_<char>-vs-<char>`. `sanitizeNameToken` then maps anything outside ASCII letters and digits to `-`. |

The manifest timestamp changes meaning on the way through. The game process sends the set's start time. `handleFreshFrame` reads it into `setStartUnix`, names the set folder from it, and overwrites `recordedUnixSeconds` with the game's own start time before writing. So every game in a set shares one folder even across midnight, and each file records when that game began. If a directory cannot be created, the host falls back to its parent rather than losing the game.

### Battle log

`battles.q99log` sits at the replay root. It is UTF-8, LF-terminated and TAB-separated, one header line then one `BattleLogRowV1` per game. Free text comes last and readers ignore unknown trailing fields. The host appends a row after a successful `finalize`, including abnormal ends, unless the `battleLog` setting is off. Failure to append is ignored, because the replay is already safe.

`appendBattleLogRow` uses `FILE_APPEND_DATA` so the offset is set by the write itself. It checks the last byte first and adds a line feed if the previous write was torn, because a merged line would still decode as a valid row.

The log is an index that can be rebuilt. `rebuildBattleLogFromLibrary` walks the scanned library and adds rows for game UUIDs it does not know, inferring `mySide` by comparing lane names to the local nickname. `summarizeBattleLog` resolves duplicates per game UUID with `preferredDuplicate`, which prefers a row with a local side.

`mySide` decides what counts. `kBattleSideOne` and `kBattleSideTwo` can produce a result. `kBattleSideSpectated` and `kBattleSideOffline` count as play time only (`battleGameVerdict` returns `NoLocalSide`).

**Format rules.** The order of the `Field` enum is the format: append only, never renumber. `splitFields` stops at the fields it knows, so an appended field costs an old reader nothing and keeps version 1. `kSchemaVersion` changes only when an existing field changes meaning, units or encoding, and `decodeRowView` then refuses those rows rather than putting a misread value in a win rate. The line from `battleLogHeaderLineV1` is a record kind no reader claims, so it is skipped by the same rule as a future kind, and two casters racing on an empty file cost only a duplicate header.

The line feed is the commit marker. `decodeBattleLogV1` discards a tail with no LF, and skips any complete line it cannot read while continuing past it. Numbers are parsed by hand so a bad field rejects the row: a row that silently read 0 would become a game played at the epoch. Decoding does not judge meaning: an unknown `sourceKind` or an odd side still decodes, because what counts as a result belongs to the stats layer and refusing would discard history the day a new kind ships.

Free text needs no escaping because `sanitizeInto` removes TAB and LF. It replaces each control byte with one space rather than collapsing runs, keeps bytes from 0x80 up so a Japanese nickname survives, and never cuts a UTF-8 sequence in half. Reads sanitize too, at the same bound, because the file is outside data; doing so is idempotent, so a row this encoder wrote round-trips. Time is stored as integer frames because the tick rate is 60.60 Hz and seconds would bake that into history. There are no floats and no locale, since a log can be read on a machine that did not write it. `battleLogSecondsFromFrames` widens to 64 bits first, because a corrupt row can carry any `uint32_t`. `readBattleLogFile` refuses a file over 16 MiB rather than stall on it. The file is opened shared for reading and writing, since the statistics page reads it during recording and a library can sit on a network drive shared by two casters.

Rows outlive their replays: "you played this" stays true after a replay is trashed. `relativePath` is composed from the set folder rather than cut from the final path, which would put the player's home directory into a file they may share.

**How the numbers are built.** `summarizeBattleLog` is pure aggregation, and it reads nothing from input order, because two casters can interleave one file. It folds duplicates first, then adds rows to sets, opponents and characters, and only then scores each set, since a set's result needs all its games.

| Rule | Why |
|---|---|
| A set is won by winning more of its games; equal is a draw | The game has first-to-N for rounds, none for games. This is a product judgement, not a recorded fact |
| Only `kBattleSideOne` and `kBattleSideTwo` count as results | Offline VS has two of you, and counting it walks a lifetime win rate towards 50% |
| The side is told, not guessed | `battleLogLocalSide` comes from the session. Matching a nickname would reattribute history on a rename. Only the library rebuild has to guess, and only when names are unambiguous |
| The winner comes from `winningSideMask`, never the tally | Rounds are -1/-1 when the end record contradicted itself; reading that as 0-0 invents a draw |
| `setKeyFor` carries a namespace | An all-zero set UUID identifies nothing. Without the third member every such row would merge into one set spanning years |
| `preferredDuplicate` decides from the rows alone | When both rows have a local side, the log is shared by two opponents. First-appended-wins would read one set from A's chair and the next from B's, and turn 2-0 into 1-1 |
| Seconds are converted per row, then added | Otherwise rows would not sum to the table's own total |
| `countedGames` includes draws, `battleWinRateTenths` does not | The first is the size of the W-L-D line; a draw is not a game anybody lost |
| `kBattleCharacterUnknown` gets no bucket, and is not a switch | It means the row did not say. Switches compare with the set's first game because row order is not guaranteed |
| A spectated set has an empty opponent | `hasLocalSide` tells the UI to write its own words, which can be translated |
| Averages and longest-set use only sets with a local side | They must describe the same population. `summarizeAgainst` filters before summarizing for the same reason |

### Catalog and browser

`ReplayScanner` runs a worker thread that walks the replay root and publishes immutable `ReplaySnapshot`s through a `shared_ptr`, in batches of `loadChunkFiles`, so the UI fills in progressively.

V2 files are parsed on every scan with `retainTimeline` and `verifyFileDigest` off. Every chunk is still walked and CRC-checked and the whole file is still hashed for `validatedSourceRangeDigest`, but the stored file digest is not compared, and `fileDigestVerifiedV2` stays false for scanned entries. `ReplayCatalogCache` (`catalog.bin`) serves set-format files only. `.trash` is skipped unless `includeTrash` is set.

`ReplayBrowserModel` holds no UI. It turns a snapshot plus `ReplayAnnotationStore` into `BrowserRow` and `BrowserDetails`, and answers `actionState` with a typed `ActionDisableReason`. `replays_ui` in `src/client/Main.cpp` draws it, so the logic can be tested without a console.

- **Sorting.** `SortKey::GameOrder` is the default. It groups by series and orders each group by `gameOrdinalInSeries`, placing groups by their newest game (`applyQuery`). Folder scoping uses `ReplayFilter::directoryScope` against `relativeDir`.
- **Compatibility.** `browserCompatibilityV2` calls the same `evaluateReplayCompatibilityV2` that launch uses. A valid but unsealed file shows as incomplete and not watchable.
- **Annotations.** Favourites, titles, tags and notes live in `annotations.q99a`, keyed by UUID. Replay files are never edited, so their digests stay valid.
- **Trash.** `doMoveToTrash` moves the file under `<root>\.trash`, mirroring `relativeDir`, and keeps one undo.
- **Verification.** "Play and check" (`VerifySimulation`) plays the file in the normal visible game window (only `replay verify --simulate` on the command line runs headless), and records success with `recordSimulationVerification` only for `VerifiedCompleted`. "Check this file" (`doVerifyFile`) calls `ReplayParser::parseFile` from the set-format parser. The complete check for a V2 game is the one `loadReplayPreflightInputV2` runs before playback.

**Why the scan never re-sorts.** Enumeration opens nothing, since `FindFirstFileW` already returns write times; parsing is the slow part. The scan orders everything by write time then path, and loads in chunks. A partial snapshot is thus a true prefix of the finished one, and rows do not move while the player reads them. A final re-sort would break that; display order belongs to the model. Each chunk gets its own generation, and generation and snapshot change together under the lock. A consumer that sampled a new generation against an old snapshot would never fetch the real result.

`maxLoadedFiles` defaults to no ceiling because chunking makes a big library cost patience rather than correctness, and a cap would hide old replays from search. Anything it drops is counted in `filesBeyondLimit`, since "gone" and "not loaded" look the same in a list. `kMaxScanDepth` allows two levels of grouping beyond the product's own source and set folders; reparse points are not followed. `moveToTrashDirW` renames a trashed file to `<name>.q99r.<stamp>.bak`, so that form is accepted only under `.trash`. `directoryScope` is deliberately outside `active`: opening a folder must not read as a filter someone has to clear.

**The browser states recorded facts and nothing else.**

- A V2 entry's set-format `manifest` is default-constructed and never decoded. Rosters, names, sides and source kind must come from `startV2`, and only once the parser actually read that start. A default start reads as offline and would relabel every unreadable file.
- `hasEndV2` exists because a default `ConfirmedGameEndV2` is indistinguishable from a real 0-0. The Rounds cell uses `roundTallies`, latched at the final transition and inside the canonical digest, always side 0 first.
- `winningSideMask` of zero cannot separate an abort from a draw, so `noWinnerTextFor` reads the terminal reason. Only `TerminalReason::Completed` is a draw. A file's index in its set is not a standing: nothing records how earlier games went, so no set score is derived.
- `roundsToWinMinusOne` is quoted only up to `kMaxRoundsToWinMinusOne`. The parse gates do not range-check it, and native fact: the game's round-count table has three entries.
- Player names come from the recorded lanes. The native display-name fields are written empty and are code-page tagged, so reading them would give mojibake. `playerTokensFor` is the one source for titles and outcomes, and it qualifies only colliding names.
- "Can I watch it" and "how well is it checked" stay separate. `ContentMismatch` shows as watchable because a root difference is a prediction; `prepareReplayV2` and the frame proof remain the authority. `identityPending` marks rows as still being checked until the identities exist.

**Safety lives in the model, not the renderer.** `requestAction` cannot perform a destructive action; only `confirmAction` can. `revalidate` refuses with `FileChangedSinceScan` if the file differs from its scan-time identity. `PermanentPurge` is denied with `NotInTrash` outside the Trash, so no API path destroys a library file on one confirmation. All file-supplied text passes through `sanitizeForDisplay`, so no replay can emit a terminal escape sequence.

### Launching playback

Three gates sit between a file and the game:

1. **`ReplayParserV2::parseFile`** with `allowUnsealedPrefix` false and `verifyFileDigest` true. It retains the full timeline.
2. **`makeReplayPreflightInputV2`** requires a sealed file with verified digests, an attested compatibility identity and a stable file identity. It rejects files with no attestation, because nothing can prove this build would reproduce them.
3. **`prepareReplayV2`** rebuilds the timeline from the parsed ticks and evaluates compatibility against `ReplayRuntimeIdentityV2`. It refuses unattested identities, identity mismatches and any feature flags other than exactly `kFeatureCompatibilityIdentityV3`, so team and CPU recordings cannot play. The whole game must fit in `kMaxPlaybackPrebufferFramesV2`.

Verification mode is chosen here and must agree on both sides, which is why `PreparedReplayV2::verificationMode` and the session both apply the same demotion:

| Mode | When | Proof |
|---|---|---|
| `ByteExact` | First game of a session, current state schema | Every pre/post state hash must match |
| `BehaviourVerified` | `gameGeneration > 1`, or a superseded state schema | Hashes skipped; lifecycle, dispatcher result, inputs, digest chain and terminal still enforced |

Rematches are behaviour-verified because a rematch starts on top of the previous game's memory, and a fresh boot cannot reproduce that byte for byte.

**`ReplayBrokerV2`** owns the caster side of the pipe. `listenBeforeInjection` runs from the launcher's suspended-process callback, so the pipe is proven to exist before the DLL is injected. `serve` waits for `PlaybackHello`, sends the prepared game as `PlaybackSessionInfo`, `PlaybackGameStart`, `PlaybackPalette`, `PlaybackFrames`, `PlaybackEvents` and `PlaybackEndpoint`, then waits for `PlaybackResult`. Playback is fully prebuffered: the file is not read again after launch.

**What compatibility can forgive.** Every other contract field describes what the state is, so a difference means a different simulation. The exceptions all concern how a proof is written:

| Difference | Handling | Are state proofs enforced? |
|---|---|---|
| `protocolVersion` | Neutralized for replays: it names a live wire dialect, and playback speaks none. Comparing it would orphan the library on every protocol bump | Yes |
| `stateHashAlgorithmId` | Membership in `replayStateHashAlgorithmSupportedV2`, not equality. The runtime keeps every primitive it has written; removing one makes those recordings unwatchable | Yes |
| Superseded `stateSchemaId` | Plays, demoted to `BehaviourVerified`, because the hashes cover different bytes | No |
| Content-root membership (`legacyRoots`) | Local roots recomputed under the older rule from this install. A real `.chr` difference still fails. Playback only: live peers both prove under the current rule | Yes, a root is not an input to a state hash |
| `contentManifestRoot` alone | Forgiven only when the file's attested identity is accepted, which proves the changed bytes lie outside the verified set | Unchanged |

`evaluateReplayCompatibilityIdentityV3` tries these allowances together, because `compatibilityMismatch` reports only the first differing field: allowances tried singly would miss a recording needing two. It substitutes values only when the recorded schema is one this build recognises, and after substituting the whole comparison must come back clean. Nothing is waived field by field.

The whole-directory root is checked after every schema field, so a schema difference is never reported as a content problem. It covers everything except `ContentManifestPolicy` exclusions, and a file the product itself rewrites inside the game folder must go into `excludedNames` or `excludedDirectories`, or each write reclassifies the library as a content mismatch. `.cache` holds the content-hash cache for exactly this reason. Results carry `detail` for the player and `technicalDetail` for the log, since the engineering wording does not tell a player whether their netplay version or their game files are at fault.

**The broker holds the runtime to the prepared result.**

- `verificationMode` includes the superseded-schema demotion, so it agrees with what the runtime derives. The returned reason must match in both directions: a rematch claiming byte-exact and a first game claiming behaviour-only are both refused, so the distinction cannot be lost silently. `CompletedWithDivergence` is exempt, since it can occur under either mode. It counts as played through, never as a verified reproduction, because the viewer did watch the whole recording.
- The echoed contract is compared with `replayContractAgreesForPlaybackV2` against the recorded contract, not the installed one. The launcher binds the runtime to the file's content root, so an attested drift would otherwise be refused after a fully verified playback.
- `resultTimeoutMs` is a liveness deadline, not a duration limit. Each accepted `PlaybackStatus` and `PlaybackCommandAck` re-arms it, and the runtime reports at least every 250 ms while holding. So a paused or quarter-speed replay never times out, while a silent runtime does. Only frames that passed `bindingValid` and their decode can re-arm it.
- `overlappedResult` honours whatever the kernel transferred after `CancelIoEx`. The completion can beat the cancel, and reporting a timeout would lose one whole message from a sequence-bound stream.
- `PlaybackResultReason` values are append-only. When adding one, check the decoder's upper bound: a reason it refuses drops the channel and shows as a closed pipe, with a good result lost on the wire.

**Why a rematch cannot match byte for byte.** Unallocated actor-pool slots and free-running counters live inside the hashed regions, but the simulation never reads them. A rematch inherits those values from the previous game and a fresh boot cannot reconstruct them, so the state hash can never match while the simulation still reproduces exactly.

**A superseded schema is the same situation.** The simulation can still reproduce, but this build's hashes cover different bytes. `ReplayPlaybackBindingV2::supersededStateSchema` is passed to the runtime rather than re-derived, because the schema ID lives in the identity envelope that only the caster parses. The flag can only demote `ByteExact`, and no caller can request a weaker proof, so a recording can never be held to less than it supports.

**The caster checks the runtime's claim.** The result reason must fit the mode the caster selected: `ReplayBrokerV2` refuses `VerifiedBehaviourOnly` for a byte-exact game, and any other verified reason for a behaviour-verified one. `VerifiedBehaviourOnly` carries no endpoint constraint, because a rematch can end normally or abnormally like a first game. Its other result fields are unchanged, so nothing downstream loses a fact; it only stops claiming a state proof that was not performed.

**The hash algorithm is negotiated by membership.** Every other contract field describes what the simulation is and must be equal. `stateHashAlgorithmId` only says which primitive produced a file's hashes, so `replayStateHashAlgorithmSupportedV2` asks whether this build can still compute it, and `replaySelectRecordedStateHashAlgorithm` switches the rollback manager to that primitive for playback. Removing an entry makes every recording that used it unwatchable. `replayContractAgreesForPlaybackV2` normalizes a copy of the recorded contract and compares the whole struct, so a new field is strict by default and only this one exemption is written down.

**Contract tables.** `ReplaySimulationContractV2` keeps the qualified executable digests as literals because `qoh99_replay` is a leaf library with no dependency on the native model. `ReplayContractDigestsMatchQualifiedProfiles` in `test_build_signatures.cpp` catches drift from the generated profiles, and the table's order means nothing. `isQualifiedExecutableSha256V2` is not an admission decision: the caster's installation gate and the DLL's host-image gate run long before it. `nativeBuildIdentity` is a compact label made from the first eight digest bytes, and `executableSha256` stays the authority.

### Playback: the frame proof

`ReplayPlaybackSessionV2` owns the timeline and judges each frame. `GameHooks` observes and ticks:

```
replayPeekFrameV2                recorded frame, is it final?
replayBeginFrameV2  -> beginFrame   compares pre-hash, lifecycle, lane mask; latches actions
native dispatcher runs              input reads return latchedActions()
replayCommitFrameV2 -> commitFrame  compares post-hash, lifecycle, dispatcher result
```

Two severities matter:

- **Divergence** (`noteDivergence`) covers state hash, lifecycle and dispatcher differences. It is counted, the first one is kept, and playback continues so the viewer still sees the game. The result is `CompletedWithDivergence`.
- **Structural faults** (`finishTyped`) cover wrong frame number, wrong lane mask, unsupported input domain or a missing presentation serial. Playback stops, since it cannot be executing this recording.

Input isolation is part of the proof. Playback actions come only from the latch, and `noteDeviceBoundaryCall` faults with `DeviceIsolationViolation` if device input is touched.

The final frame has one more step. For an interactive run, the serial for the final frame is reserved before entering the dispatcher, so the session is `TerminalArmed` before the game can present. `acknowledgeFinalPresentation` then completes it. A result is therefore never reported for a frame nobody saw.

**Past the end.** A recording stops at the first win-screen tick, before the banner has settled. `replayWinScreenPendingV3` delays the result while `replayWinScreenAction` advances extra unrecorded ticks until the banner settles (`Hold`), giving up with `Fail` after 600 ticks. With `kReplayHoldFinal`, the result waits another second (`replayFinalHoldElapsed`) so the last frame is visible. A win-screen failure rewrites the reason to `InternalFailure`.

**What stays fatal, and why.** A replay is fundamentally its recorded input stream and the order of that stream. Diverging state can still be watched, but feeding the game different inputs than were recorded cannot, so frame numbering and the lane mask stop playback while state, lifecycle and dispatcher results are only reported. Pre-state differences are reported for the same reason: once a run has gone its own way, every later frame starts from a state the recording never saw. `CompletedWithDivergence` is its own outcome, so it cannot be read as a faithful reproduction or as a failure to play. `isPlayedThroughPlaybackResultReason` counts it as played through, and `isVerifiedPlaybackResultReason` does not count it as verified.

**Frame zero has two separate checks.** `completeBootstrap` always requires the declared `frameZeroPreStateHash` to equal what frame zero recorded, and a mismatch is `BootstrapPreHashMismatch`, because that is the recording disagreeing with itself. It also always requires the initial phase and frame-zero lifecycle to match. Comparison against the freshly booted image (`bootstrapFingerprint` and the observed pre-state) happens only in `ByteExact` and is reported as divergence. A differing boot image means the viewer is watching this build's simulation, which is worth saying and not worth refusing to show.

**Transport cannot affect the proof.** `ReplayPlaybackSessionV2` has no pause, step or speed state. These live in `ReplayRuntime` and decide only how many `peek`, `beginFrame`, `commitFrame` transactions run per outer slot. Every comparison, the digest and the terminal checks see the same call sequence at 0.25x, at 8x or one frame at a time.

`replayTransportTicksForSlotV3` plans a slot from a permille credit that counts slots, not wall-clock time, so 0.25x advances on exactly every fourth slot forever and a slow machine plays slower rather than skipping anything. At 2x and above it runs `hiddenTicks` plus one presented tick. Below 1x the idle slots set `slowMotionHold` and re-present the previous frame without running a native tick. `replaySpeedInitialCreditV3` primes the credit so that the first slot after a reset advances instead of waiting a full cycle.

**When a hold is legal.** `transportMayHold` is true only in `Ready` with no result, the same phase in which `peek` offers a tick, so "the viewer may stop here" and "a tick is available here" cannot disagree.

| Phase | Why a hold is refused |
|---|---|
| `Bound` | The bootstrap proof has not been made; a hold would stall it and hold nothing visible |
| `FrameInFlight` | Mid-transaction; a slot never ends here |
| `TerminalArmed`, `AwaitingFinalPresentation` | The final frame is dispatched and its presentation wait is budgeted. Exhausting it ends with `PresentationAckTimeout`, which is not a verified reason, so pausing here would burn an earned proof |
| `ResultReady`, `Faulted` | The proof has concluded; there is nothing left to pace |

Commands fail closed: a refused one does nothing, and on the wire it is answered with `RejectedPhase`. `transportHoldingV3` also requires `transportMayHoldV3`, so a pause already set lapses when the session leaves `Ready` and cannot starve the final presentation.

### Viewer transport

Pause, frame-step and speed may change only how many times a slot runs the verified peek, begin and commit transaction. `replayTransportTicksForSlotV3` is the only place pacing is computed, and nothing in the transaction reads the transport fields. A paused slot runs no transaction, so it skips no comparison. A step runs exactly the transaction a playing frame runs, and a speed rung changes only the count.

- **Slot arithmetic.** The fractional remainder is carried in `creditPermille`, so 0.25x is exactly 0,0,0,1 with no drift. `replaySpeedInitialCreditV3` primes slow rungs so a key press shows at once. `paused` is passed by value, so spending a step cannot resume. A step with no recorded frame left stays queued rather than being consumed by a slot that ran nothing.
- **Commands.** Step defines the paused state, and presses saturate rather than wrap. Changing speed leaves `pausedV3` alone, because an implicit resume would advance the verified path from a presentation key. `nextPlaybackSpeedV3` saturates at the ends of `kPlaybackSpeedLadderV3` and fails on an unsupported rung rather than repairing it. Relative steps are resolved on the game thread against the live rung, since a wrapper working from polled status would lose presses. A new producer must reuse the wire kinds that `isPresentationOnlyPlaybackCommandKind` lists rather than add a second encoding of the same intent. That check also keeps restart and seek commands out of a presentation-only control.
- **Holds near the end.** `transportMayHold` is true only in `Ready` with no result. The wait for the final presentation is budgeted at 300 slots, and exhausting it finishes with `PresentationAckTimeout`, which is not a verified reason. `replayPlanOuterSlot` therefore returns from that phase before the transport arithmetic. The two rules must stay agreed, or a viewer hold could burn a proof already earned.
- **Slot cost.** `slowMotionHold` is the only ordinary zero-tick case charged a full presentation period, because that wall time is the slow-down. Viewer pause and the terminal wait stay on the roughly 1 ms hold. The terminal budget is counted in slots, and pacing it would stretch 300 slots from about 0.3 s to about 5 s.

**Where the keys come from.** Playback may not read any device: `replayNoteDeviceCall` on a suppressed path fails the run. The one legal reader is the wrapper's render-side message pump, so the DLL registers with `EFZDX9_RegisterReplayTransportProvider`, retrying every 500 ms because `ddraw.dll` may load later. The render thread only reads a `ReplayTransportStatusV2` from a seqlock and pushes to the ring behind `replaySubmitPresentationCommand`. It must not touch the session, which the game thread destroys, and a failed read just keeps the previous view. The wrapper must read paused and speed back from that snapshot, since a private copy would drift the first time a full ring dropped a submit. The local path sends nothing on the wire: wire commands need an id and an ack, and the two producers meet only at the transport fields, behind the same gates.

`stopReplayRuntime` must keep its order: publish the inactive snapshot, de-register, then destroy the session. The input-display and transport registrations are separate because they are independently versioned ABIs, so the input viewer works on a wrapper that has only its export.

**The input viewer cannot disturb the proof.** `publishInputDisplayLatch` runs only after a frame is accepted, never at commit, which clears the latch and would publish neutral on every frame. It reads the latch rather than calling `replayInjectedAction`, whose read counter tests depend on, and it ignores publish failures. Stored masks are compact and the display ABI is logical, so `logicalFromCompact` is mandatory; reading compact as logical would draw directions as buttons. `latchTick` never resets, so `inputDisplayGeneration` is what discards the previous recording's history.

Native fact behind `replayInjectedAction`: the game's input path reads more than once per frame, so the value is latched per tick and every read in that tick must return the same mask. A lane outside the active mask injects neutral, never a stale or predicted value.

### Seek cache

Seeking is restore-then-resimulate. `ReplaySeekCache<ReplaySeekSnapshot>` in `src/GameHooks.cpp` holds in-memory snapshots. Each one combines the native state, audio checkpoint, a `ReplayPlaybackSessionV2::Checkpoint`, and the pre-hash and lifecycle at that frame.

Capture happens just before `replayBeginFrameV2` on gameplay frames when `wants(frame)` is true: non-zero, a multiple of the stride, and later than the newest entry. The stride starts at 16. When the budget (256 MiB by default) would be exceeded, the stride doubles and entries off the new stride are dropped. So the cache covers the whole played range at a coarser and coarser grain, and never grows without bound. Allocation failure skips the checkpoint and loses nothing but speed.

A seek request (`requestNavigationV3`, from `SeekFrame`, `Restart` or `StepBack`) is refused during takeover, during another navigation, or for a target at or past the endpoint. `StepBack` targets `nextRecord - 2`, because `nextRecord` has already passed the visible frame. Then:

1. `ReplayNavigation::request` sets `restorePending`, and the runtime holds the tick loop.
2. `GameHooks` calls `find(target)` for the newest entry at or before the target, and restores it. With no entry it restores `g_replayStartSnapshot`.
3. For a checkpoint, it recomputes the live hash and lifecycle and requires them to equal the stored values. A restore that does not reproduce is a failure, not a warning.
4. `replayCompleteRewind` restores the session cursor and divergence counters, or restarts and re-checks the restored start state against the recorded bootstrap. Failure reports a broker failure.
5. `discardAfter` drops entries past the restored point. They are regenerated on the way forward.
6. `ReplayNavigation::plan` spends up to 16 hidden ticks per presented slot, and presents only on the slot that reaches the target. `committed` pauses there. Audio during hidden ticks goes through `flushPendingMuted`.

Hidden ticks run the same `beginFrame` and `commitFrame` as visible ones, so seeking cannot skip verification. Checkpoints also restore divergence state, so a verdict after a backward seek reflects a consistent run.

`SeekMatch`, `SeekRound`, `NextMatch` and `PreviousMatch` exist in `PresentationCommand` but are not applied in V2 playback, since a file holds one game.

### Folder playback

`watchFolderAndReturn` plays a whole folder in one game window. `folderPlaybackOrder` takes the non-trash entries in that directory sorted by recorded time, then reorders each series' positions by `gameOrdinalInSeries`. A set therefore plays in game order even if its timestamps disagree, and unrelated files keep their chronological place.

The caster supplies a `ReplayBrokerNextGameProviderV2` closure. When the current game's result arrives, the broker calls it only if the result counts as played through and the recording's terminal is `Completed`. An abnormal game ends the folder there. The closure loads the next file through the same three gates, so a bad file refuses cleanly with its reason in `next.error`.

The switch is a handshake with a FIFO barrier:

1. The broker sends `PlaybackNextGame`, binding the new game's identity, counts, source digest and runtime contract.
2. The worker receives it. `ReplayRuntime` accepts it only at a completed-result boundary (`replayCompletedGame`), otherwise it halts.
3. `replayBeginNextGameV3` submits `PlaybackNextGameAck` and resets the session.
4. The broker checks the ack payload equals what it sent byte for byte, and swaps `prepared_`. Packets before the ack belong to the old game, packets after it to the new one.
5. `replayBeginNextNativeGameV3` in `GameHooks` clears native state, including `g_replaySeekCache` and the snapshots, and returns to `ReplayBootstrapPhase::WaitingForStart`.

Presentation flags carry over through `kReplayDisplayFlagsMask`. `kReplayAutoStart` is always set after the first game, and `kReplayHoldFinal` only on the last.

### Takeover

Takeover lets the viewer play either side from any frame of a replay, then return. It is a speculative branch outside the proof: while it runs, the `ReplayPlaybackSessionV2` is not touched. Its `nextRecord` stays frozen at the branch point, and nothing is compared.

`ReplayTakeoverController` is pure state, with phases `Inactive`, `Countdown`, `Active`, `RestorePending` and `Faulted`.

**Entry.** `TakeoverP1` or `TakeoverP2` is accepted when `takeoverAvailable`, not navigating, past bootstrap, not headless, and `nextRecord` lies between 2 and the endpoint. `begin` records the branch frames. `GameHooks` sees `replayTakeoverNeedsSnapshot`, starts the input runtime (playback otherwise has no device access) and captures a full snapshot with `replayTakeoverCaptureSnapshot`. If that fails, takeover is disabled for the game. A countdown of `kReplayTakeoverCountdownFrames` follows, at normal speed.

**Each active frame.** `replayTakeoverBeginFrame` runs `capture` for the live input, then `resolve`, which starts from the recorded tick and overlays any lane with a stored input. The tick then runs with the latch as usual, and `replayTakeoverCommitFrame` advances.

**Storage.** `slots_` is one `uint32_t` per branch-relative frame, one 16-bit action per lane, with `kNoInput` meaning use the recording. `prepare` sizes it to the endpoint plus `kReplayTakeoverContinuationFrames`, capped at `kMaxTimelineFrames`.

- Delay (0 to `kReplayTakeoverMaximumDelay`) is applied at capture: input lands at `elapsed + delay`. Raising it fills the gap with the current input. Lowering it writes nothing on the capture where the target falls behind the last written frame, but that capture moves the write position back to the target, so later captures overwrite the frames already written after it.
- `TakeoverRetry` and a side switch keep the slots, so the previous attempt plays back. Switching to the other side lets you practice against what you just did. `test_replay_takeover.cpp` pins this.
- Only `Exit`, `begin` and `reset` clear the slots.

**Past the endpoint.** `resolve` takes a null recorded tick, derives the source frame from the branch, uses `takeoverFallbackLaneMask` and flags `beyondEndpoint`. The other side receives zero input unless it holds stored input.

**Leaving.** Retry, switch and exit all go through `RestorePending`, and `completeRestore` runs after `replayTakeoverRestoreSnapshot`. Retry and switch restart the countdown; exit returns to `Inactive` and playback resumes exactly at the branch frame. A failed restore is `Faulted`, since the game state cannot be trusted. If the branch reaches a lifecycle boundary (`rollbackEligible` false, or the round ending), `GameHooks` calls `replayTakeoverRequestSafetyExit`, because the branch cannot safely run past the round on an unproven state.

### Where to look when changing things

| Change | Check |
|---|---|
| A field in a chunk or manifest | `src/replay/ReplayCodecV2.cpp`, then decide whether `feedCanonicalManifestV2` should include it. Anything fed there changes canonical digests. |
| Simulation behaviour | `src/replay/ReplaySimulationContractV2.h` and the compatibility identity. Otherwise replays diverge with no explanation. |
| Wire messages | `src/replay/ReplayIpcWire.h`, `isKnownMessageType`, and both `ReplayIpcWorker` and the host or broker |
| Timeline rules | `ConfirmedTimelineValidatorV2` only; every path shares it |

Tests live beside the code in `src/replay/tests`, `src/client/replay/tests` and `src/game/replay/tests`. `test_replay_fuzz.cpp` feeds malformed files to the set-format `ReplayParser`, `test_replay_writer_recovery.cpp` injects writer faults through a `MemoryFileSink`, `test_replay_storage_v2.cpp` injects V2 writer faults through `setFaultInjector`, and `test_replay_seek_cache.cpp` pins the stride behaviour.

**Adding a `PlaybackResultReason`.** `encodePlaybackResult` and its decoder both hold a literal upper bound that must track the last enumerator. Missing it does not degrade gracefully: the runtime refuses or the peer rejects a perfectly good result, the channel drops, and the caster reports only `replay IPC channel closed`. Look here first when that message follows a run that seemed to finish.

## Training mode core

Training is an offline sandbox built on the game's real simulation. The game still runs its own fight code. Training decides which frames may simulate, what input each fighter reads, and which values it may rewrite between frames. Any state it cannot vouch for fails closed.

The code has two layers:

| Layer | Files | Character |
|---|---|---|
| Pure policy | `src/training/*.h`, `src/training/*.cpp` except `NativeRecoveryQualification.cpp`, built as the `qoh99_training` library | No game memory. State machines and plans over plain structs, tested by `qoh99_training_tests` in `src/training/tests/` |
| Integration | `src/training/TrainingGameIntegration.inl`, included once by `src/GameHooks.cpp` | Reads and writes fighters, installs detours, feeds the pure layer |

Most integration bugs come from getting the order of operations wrong, so start with the frame.

### One frame

Three entry points drive everything:

- `trainingOuterTick` runs from the outer loop. It registers the overlay providers, tracks the scene, sets `TrainingLifecycleState`, starts a new generation on return to character select, and calls `trainingPublishStatus`.
- `hkTrainingStepGate` wraps the game's per-frame gate. That gate returns 0, 1 or 2, and only 2 lets the frame simulate.
- `hkInputReadResolved` in `src/GameHooks.cpp` asks `trainingOverrideActive` and returns `trainingOverrideMask` when it says yes.

The gate hook first finishes the previous frame by calling `trainingFinalizePendingStep`, then calls the native gate and passes the result to `trainingAdmitAfterNativeGate`. A native 2 only becomes an admitted step if all of the following happen, in this order:

1. The scene is the live match, and `trainingQualifiedGameplayGate` passes.
2. `trainingApplyConfigOverlay` re-forces the match config and clears the native pause and freeze bytes. `trainingRepairHumanOwnership` undoes any drift in who controls which fighter.
3. A baseline is captured if none exists (`trainingCaptureBaseline`), and `trainingInvariantsHold` must pass.
4. `g_trainingInput.pollActions` turns held hotkeys into edges, on the game thread only.
5. If an edge can rewrite the world (`trainingImageMutatingActionMask`), `trainingCaptureSnapshot` fills `g_trainingScratch` first.
6. `g_trainingContactPolicyLatched` copies the requested contact policies, so no policy changes in the middle of a step.
7. `trainingDrainCommands`, `trainingApplyActionEdges`, `trainingConsumeRefillRequests` and `trainingConsumeResourcePolicies` apply the mutations. Two held-level actions, `RefillAll` and `ChargeExFraction`, write EX later, in step 10, after the re-capture in step 8.
8. Config, ownership and invariants are checked again, and `g_trainingScratch` is re-captured. This image is the known-good state for the coming step.
9. `trainingAdmitPausedFrame` consults `TrainingHold`. A held frame returns 1 with zeroed input.
10. `g_trainingStepSerial` advances, `trainingComposeInputForStep` produces the final masks, and `g_trainingStepPending` is set.

When the step ends, `trainingFinalizePendingStep` re-checks invariants. On success it runs `trainingAdvanceCombatToolsPostStep`, which holds the triggers and frame advantage, and discards the scratch. On failure it restores the scratch through `trainingRestoreScratchFailSafe` and sets `g_trainingMutationFailed`. It also runs from `hkTrainingMatchScope` and from the outer tick if the scene changes, so a pending step cannot be left unfinished.

**Fail-closed state.** Once `trainingFailMutation` has set `g_trainingMutationFailed`, `trainingAdmitAfterNativeGate` holds every frame instead of admitting it. `trainingDrainFailedRecoveryCommands` rejects every command except `TrainingCommandKind::ReturnToCharacterSelect`. The way out is back to character select, which starts a new generation.

**Pausing.** `TrainingHold` keeps two independent reasons, `TrainingHoldUser` and `TrainingHoldMenu`. Neither writes the native pause byte. Returning 1 from the gate skips the game's rendering too, so a held frame calls `trainingRepaintHeldFrame`, which uses the wrapper's `EFZDDRAW_PresentHeldFrame` export. `requestFrameStep` grants one credit, and `admit` spends it.

**Endless round.** The invariant baseline covers round result, round progress, outcomes and wins. A step that changes any of them is a failure, not a round ending. `hkTrainingDamage` stops damage from reaching that point. It lets the native damage code run, then floors effective life at zero and restores the fatal-cause fields it had saved.

**Why a held frame must present.** The game renders from inside its gameplay tick: the display list is built during the simulation body and flushed near its end, and that flush issues the completed-frame blit that drives the wrapper's present. A held frame returns far earlier. The native game does not mind, because DirectDraw leaves the last image on screen, but the overlay is composited during that blit and would vanish behind a frozen picture. So "held" has two separate outputs, simulate nothing and present once, both carried by one decision (`TrainingHoldAdmission::presentHeldFrame`). No caller can infer this from the return value, because every exit of the native tick returns 1.

The native alternatives do not work. The native freeze byte does render without simulating, but `trainingApplyConfigOverlay` clears it and `trainingQualifiedGameplayGate` refuses to run while it is set, so the hold could never be left. It also opens the native developer menu and decrements state that is part of the gameplay hash on every paused frame. Re-running the last display list would re-fire its callback nodes and push a paused frame through the frame-commit protocol.

**Order within the gate.** `setMenuCaptured` runs before `admit` in the same call, on the game thread, so zero simulated frames leak between capture toggling and the hold engaging. The hold is a reason set, not a second pause, so opening and closing the menu while paused leaves you paused. A frame-step credit saturates at one and is granted and spent inside one call, so a burst of presses cannot release a burst of steps later. Outside the menu, a frame step also sets the user reason; under a menu hold it borrows the menu's reason, so closing the menu resumes. Save, load and reset work while paused because the hold check comes after every mutation and after the pre-step snapshot.

**Return to character select.** The F1 edge arms a return, but the gate would otherwise go on to re-apply the config overlay that `trainingBeginReturnToCharacterSelect` just undid, and compose input for a match that is tearing down. The gate therefore stops straight after the hotkey table, the same way it does after the command drain.

**Native F6.** The native key calls the full round reset, which refills life, wipes every actor and moves the round-state fields `trainingInvariantsHold` compares against its baseline, so it would fail training closed. The native edge stays suppressed and `TrainingAction::ResetPositionRoundStart` runs instead.

### Generations

`g_trainingGeneration` numbers one character-select-to-match cycle. `trainingBeginNewGeneration` bumps it, skipping zero, and resets macros, savestates, input arming, the hold, settings and timeline machines. It publishes the number to both the queue (`publishGeneration`) and the overlay's capture path (`g_trainingUiGeneration`). A command or menu-capture call carrying an old generation is refused. Nothing from a previous match can reach the current one.

`trainingResetPlayerSettings` is the one list of player-chosen values, used by both arming and a new generation. A value belongs there if a player chose it and it has a defined default. Timeline state belongs to `trainingResetTimelineMachines`. The menu reads the runtime rather than a mirror, so a setting that survives a generation appears as a stuck menu row. A new generation also clears the learned reversal leads and the player swap, and resets the input display, because the step serial restarts at zero and surviving history would carry serials the next steps reuse.

### The overlay boundary

The wrapper (ddraw.dll) has its own copy of the types in `ddraw_wrapper/render_dx9_imgui.h`, under `Qoh99` names such as `Qoh99TrainingStatusV1` and `Qoh99TrainingCommandV1`. `qtraining::TrainingStatusV1` and `qtraining::TrainingCommandV1` in `src/training/TrainingTypes.h` are the other half. `trainingTryRegisterUi` hands three callbacks to the wrapper's `EFZDX9_RegisterTrainingUiProvider` export, retrying every 500 ms until the wrapper is there:

| Callback | Direction | Rule |
|---|---|---|
| `trainingCommandEnqueue` | overlay to game | Nonblocking enqueue only. No game work |
| `trainingStatusProvider` | game to overlay | Copies a released tier of the newest snapshot |
| `trainingMenuCapture` | overlay to game | Sets the capture atomic and, when the value changes, disarms the selection-screen input gate. `setMenuCaptured` latches it on the game thread |

**Commands.** `TrainingCommandV1` is 32 bytes: API version, size, generation, a monotonic `commandId`, `commandKind`, `slotOrSetting`, `value`, `flags`. `TrainingCommandQueue` is a single-producer, single-consumer ring of `kTrainingCommandCapacity` entries. `tryEnqueue` checks size, version, generation, id order, `isValidCommandKind` and `validCommandPayload`, and returns a `CommandIngressResult`. `tryDequeue` re-checks the generation, because it can change between validation and publication.

On the game thread, `trainingDrainCommands` applies at most `kTrainingCommandsPerStep` commands per frame through `trainingApplyCommand`, and each gets a `CommandAckResult`. Two rules apply here:

- The first command in a step forces a recovery snapshot before it runs.
- A kind for which `commandOwnsNativeImageTransaction` is true (`ResetPosition`, `ResetRoundStartNeutral`, `LoadState`) ends the drain. Later commands wait for the next frame, so they apply to a world whose state has been re-checked.

`TrainingCommandKind` values are never renumbered or reused. Retired kinds such as `SetHpRegen` keep their number and are rejected, so a stale press cannot be read as something else.

**Status.** `TrainingStatusPublisher` is a double buffer with a sequence counter. Each buffer is an array of atomic words. The game thread is the sole writer, and `read` retries up to eight times and can return false. This is why `TrainingStatusV1` must stay a whole number of `uint32_t`. Booleans are stored as `uint32_t`, and 64-bit serials are split into two words.

**Size tiers.** The status struct only grows at the end.

- `kTrainingStatusV1BaseSize` (568) is a frozen prefix.
- `kTrainingStatusV1SizeTiers` lists every released size, and `trainingStatusTierFor` picks the largest that fits the caller.
- `trainingStatusProvider` copies exactly that tier and writes it to `structSize`.
- An overlay checks `structSize` before reading an appended block. `TrainingHasStatusMessages` is one example.

The current sizes are 32 bytes for the command and 1888 for the status. `TrainingAbiIsFixedPod` in `src/training/tests/test_training_core.cpp` pins them.

To append a status field:

1. Freeze the current top tier. `kTrainingStatusV1StatusMessageTier` is written as `sizeof(TrainingStatusV1)`, and only the newest tier may be. Released tiers must be literals, because a tier defined by `sizeof` silently moves on the next append.
2. Add the field to both structs, and add a new tier to `kTrainingStatusV1SizeTiers` and to the `QOH99_TRAINING_STATUS_V1_*` macros.
3. Update the size and offset `static_assert`s on both sides, and the pins in `test_training_core.cpp` and `src/tests/production/test_production_training.cpp`.

`QOH99_TRAINING_UI_API_VERSION` changes only if a field changes meaning or a callback signature moves, never to add fields.

**Status lines.** Two kinds of message reach the HUD. A `StatusEvent` is a fading one-off: `trainingRaiseStatusEvent` stores the kind and detail and increments `g_trainingStatusEventSequence`, and the overlay notices the new sequence number. A `StatusMode` is a held state such as Recording. `trainingPublishStatus` derives it from the macro state on every publish, so it cannot go stale. Both enums, in `src/training/TrainingStatusMessage.h`, are append-only.

**Drafts and honest zeroes.** The menu's editable values are drafts, not readings. Re-seeding every frame would drag a slider out from under the mouse, so `SeedTrainingCombatToolDrafts` seeds once per generation and character. It seeds only from a live block: `triggerMoveCatalogId` is written with the configs, so 0 means "not filled yet", and seeding earlier would send a 0% chance on the first Set. When the block is not live, the seeded drafts are cleared rather than kept, because a move index only means something against the catalog it was picked from, and re-sending it after a character change would arm an index from another character's list.

Several status quantities have a legal zero, so a failed or unpublished read looks like a measurement. Rows therefore check their own tier or validity flag and say why they are blank. Each group has its own predicate on its own `QOH99_TRAINING_STATUS_V1_*` constant. Testing `sizeof` instead would read as "is the newest build", and every appended block would then disable older groups whose offsets never moved.

**Reading the recovery panel.** The DLL's eligible bit is the authority, because it evaluates terms this ABI does not carry. The per-term columns are the wrapper's own arithmetic over published fields and exist only to show which term failed, so a disagreement between the two is itself a finding. The stand-down reasons describe the last step that ran, because recovery emission only advances on real simulation steps, so while the menu holds the game they can predate its opening.

These native facts are the usual traps. The state dispatcher switches on fighter `+84` and recovery eligibility gates on `+48`, and they diverge: the airdash values 48 and 49 appear in `+48` with no `+84` case. The action-state log records them separately, and it dedupes on the real-step serial, since a held frame can present many times. The manual recovery block switches on `+156 & 0x0F`, while the terminal and throw-escape gates test `+156 & 7`, so masking everything with 7 turns a refused 9 into an accepted 1. Forward and Back are normalized against `+170` before the game records them, so a wall-tech reading needs the world side and the facing together.

The neutral test is sound but not complete: every state it accepts can act, but it also refuses many that could.

**Tiers and validity flags answer different questions.** A tier says whether the bytes exist in this binary pair. A flag such as `combatReadoutValid` says whether the native read succeeded on this publish. A failed read leaves its fields zero and clears the flag, and there is no sentinel, because every quantity has a legal zero. An overlay must show "unavailable" rather than a zero. Separately derived groups carry separate flags, so one failed read does not blank the others. The fighter window is also gated on `inMatchReady`, because the native tables stay readable after a match and would otherwise show the previous match's numbers as live. Raw fighter words are displayed as numbers on purpose, since the meaning of the action fields is not established. "Neutral (training predicate)" is a working definition and must never be labelled "actionable".

**Commands that share a word.** `RefillLife`, `RefillGuardGauge` and `RefillExMeter` carry their target in `value` with `slotOrSetting` zero. Later kinds such as `SetLifeValue` carry the target in `slotOrSetting`. This difference is part of the frozen ABI and must not be tidied up. Some commands carry several settings at once (recovery timing, meter charge). Their menu rows re-send the other fields from the runtime's status echo, never from a local mirror, because those settings can change during play and a mirror would overwrite them with stale values. Life is set in absolute units with a floor of 1, because the game's knockout test reads that number. Both guard gauges are one native call with a per-side mask, so that row always writes both sides.

**Separate providers.** The input viewer and replay transport have their own registrations and API versions (`EFZDX9_RegisterInputDisplayProvider`, `QOH99_INPUT_DISPLAY_API_VERSION`, `EFZDX9_RegisterReplayTransportProvider`, `QOH99_REPLAY_TRANSPORT_API_VERSION`). A version mismatch nulls the whole callback set, so sharing a number with training would let a training ABI break take down replay features. Replay playback also has no training runtime, so the viewer could not be folded into the training status.

- **The producer owns input history.** Rendering runs at present cadence and simulation at step cadence, and pause and frame-step separate them completely. The producer therefore folds once per step and publishes the whole ring, and the wrapper only reads. Equal step serials mean nothing advanced.
- **Off by default.** `QOH99_INPUT_DISPLAY_MODE_NONE` is 0, so a zeroed snapshot or a missing producer draws nothing. This is also what keeps the viewer off netplay, versus and spectating.
- **Transport status is never mirrored.** `Qoh99ReplayTransportStatusV2` is pointer-free because the playback session can be destroyed mid-poll. A wrapper copy of "paused" would drift the first time a full ring dropped a submit, so `ReplayTransportPoll` is the only source. `ReplayTransportSubmit` returns the status's `generation`, and a stale one is refused, so a key pressed during a session change cannot land in the next session.
- **Spectators** have no transport. `EFZDX9_RegisterSpectatorHotkeySink` receives the same command numbers.

A rejected registration is remembered so the menu can name both versions. A silently missing overlay would otherwise look the same as no producer running. Struct growth never reaches this path, because `structSize` handles it per call.

**Readouts carry licences.** Every status group that comes from native memory has its own validity flag (`combatReadoutValid`, `resourcePolicyValid` and so on), and on a failed read its fields stay zero so the overlay can say "unavailable". The flags are separate because each group is reached by a different route, and one failed read must not blank a good group. Both players must succeed before a flag is set, because one live fighter beside a frozen one looks correct on screen. `inMatchReady` is part of each measured group's licence. The game leaves its fighter table and combo pools readable after a match ends, so an ungated read shows the last match's numbers as live at character select.

The combo counters and match records are read fresh on every publish. They live in savestate regions, so a cached copy would disagree with the game after a load. Life components are published as signed bit patterns from a single `trainingReadLifeComponents` call: pending damage and current life can both be negative, and two reads could straddle a native write and publish a total that does not equal its parts. Current plus reserve minus pending is the expression the game's own death test uses. Raw readouts stay raw, and the "neutral" label names the training predicate, not a promise that the fighter can act next frame.

The menu shows the requested contact policy, not the latched one, because the latch would make a fresh selection appear to snap back for a frame.

**Size negotiation.** Rejecting a smaller caller would make `ReadTrainingStatus` fail, dropping every training tab with no message, so a stale `ddraw.dll` gets the largest released tier that fits. Only a released tier is ever produced, because copying an arbitrary length could leave a future wrapper a half-written field.

**Registration.** Publish before registering, because the wrapper may poll on the next present, and a provider answering from an unwritten buffer looks the same as one that has stopped. On teardown, publish a hidden frame and then release. The overlay keeps its last view when a read fails, so a producer that stops silently leaves boxes frozen on a live game, and a registered provider keeps the wrapper's D3D9 path open. Status, input display and hitbox providers register separately, so one rejected registration cannot decide the fate of another ABI.

**Replay uses a narrow channel.** `trainingReplayNativeHudTick` is playback's only tick into this subsystem. It registers with `hudOnly`, publishes only the HUD slice through `trainingPublishNativeHudOnlyStatus`, and drains the queue with an allowlist: every kind except `TrainingCommandKind::SetNativeHudMask` is refused with `RejectedUnavailable`, not deferred. A reset or load must not touch a game reproducing a recorded timeline, and a deferred command would fire later when training starts.

**Rules for status fields.** A tier and a valid flag answer different questions. A tier says whether the bytes exist in this pair, and a flag such as `combatReadoutValid` says whether the native read succeeded this publish. Gate each appended group on the tier it started in, never on `sizeof` of your own struct, or an older peer loses fields whose offsets never moved and is told its DLL is stale.

- Each group has its own flag because each is reached by a different derivation and one can fail alone. Life components sit in the fighter tier's bytes but are licensed by `combatReadoutValid`, because they come from the same read as the effective life.
- A failed read leaves zeros and a clear flag. There is no sentinel, because every quantity has a legal zero.
- The publisher gates these reads on `inMatchReady`, because the fighter table and combo pools stay readable after a match and would show the old match as live.
- Combo readouts are sampled live each publish, because the native structures sit in savestate regions and a cached copy would disagree after a load. The accumulators index by the victim (`p1ComboHitsTaken`) and the match records by the attacker (`p1MaxComboHitsDealt`).
- `damageEventSerial` counts hits the native body admitted, and `damageDeclinedCount` counts the entries it refused. A detour entry is not a hit, so no visible counter may advance on entry alone.
- Fields such as `p1ActionState` are raw words with known offsets and unresolved meanings. A UI may print the number but must not print "neutral", "hitstun" or "can act".
- Signed values travel as `int32_t` bit patterns in `uint32_t` slots.

**Rules for commands.** `commandMayCoalesce` allows only idempotent settings, where dragging a combo needs just the last value. Refills, `FrameStep` and the `Set*Value` kinds stay out, because merging two presses would swallow one. Payload conventions differ on purpose: `RefillLife` carries its target in `value`, and that form is frozen, while kinds needing two numbers put the target or policy slot in `slotOrSetting`. `validCommandPayload` enforces each per kind. `TrainingRefillTarget::Both` is zero, so a zeroed payload means everything rather than silently P1.

### Who drives which lane

By default P2 is the dummy. `g_trainingPlayerSwap` (`SetPlayerSwap`, accepted only while macros are idle) reverses that, so input, guard and recovery code goes through `trainingDummySlot` and `trainingHumanSlot`. Triggers do not follow the swap: `kCombatDummyOwner` stays at P2's slot, and the wakeup-reversal checks read `g_trainingCombatIdentity[1]`.

While recording, you drive the dummy's lane and your own fighter stands in the other. The dummy settings would otherwise act on nobody useful, so `g_trainingPolicyFollowsHuman` moves them. `trainingPolicySlot` then names your own fighter, and `trainingPolicyPressureSlot` names the attacker. `trainingComposeInputForStep` restarts guard timing whenever this flips.

`trainingComposeInputForStep` fills a `TrainingInputStep`, and `TrainingInput::advance` merges it:

1. Physical masks are dropped if the menu is captured or `rearmState_` is not `InputRearmState::Armed`. After a capture, input must pass through `AwaitNeutral` and `NeutralSettle` before it counts, so a button held across a menu cannot fire on close.
2. Macro overrides replace the physical mask per lane.
3. `carryMask_` removes the reset-modifier direction the human is still holding after a position reset.
4. Dummy posture is applied to the policy lane, axis by axis. `dummyOwnedAxes` names the axes the policy owns. Unowned axes keep the existing input. During playback, an axis the macro is using is released from the policy, and the whole merge happens only if `guardDuringPlayback` is set.
5. `cleanSocd` resolves opposing directions.

`advance`, `pollActions` and `TrainingMacros::advance` all take a serial and are idempotent: a repeated serial returns the cached result, and a lower one is invalid. Pure-layer state can't advance twice for one real step.

The final masks wait in `g_trainingFinalInput` until the game reads them. In the match, `trainingOverrideActive` is true only while a step is pending or the menu is captured; the selection screens have their own input gates. Finalizing zeroes the masks and clears the pending flag. A later read gets neutral only while the menu is captured; otherwise `trainingOverrideActive` is false and the read falls through to the normal offline path. `g_trainingLastComposedInput` keeps a copy for the status block.

**Two kinds of neutral.** `operationNeutral` or an unavailable lifecycle zeroes the whole pair, because a reset must land cleanly. Menu capture and a pending re-arm zero only the physical and macro lanes. While the menu owns the keyboard those bits are menu keystrokes and must not reach a fight mask, but the dummy posture comes only from settings and facing, so it contains no device fact. The dummy therefore keeps blocking while you read the menu, and through the re-arm steps after it closes. Without that, a combo that should be blocked appears to work.

**Operations are not device changes.** `resumeAfterOperation` keeps an armed machine armed after a load, reset or record toggle, because the player never let go. It forgets the SOCD memory, requires a held hotkey to be released, and suppresses only the direction the operation consumed. The neutral-rearm test covers fight controls only; if it included hotkeys, holding Reset would pin both fighters neutral because the key that closed the gate could never satisfy it.

**Ownership, not OR.** Posture replaces whole axes. ORing a guard direction into a lane where a macro or a second device holds the opposite would send a self-inflicted Left+Right into the SOCD cleaner, and under Neutral SOCD that silently erases the guard while the menu still says it is on. For the same reason `advance` rejects directions outside the owned axes, and a caller standing aside must zero directions and axes together: owning an axis with no direction deletes what the macro put there. Playback is additive and yields any axis the recording asserts, so a quarter circle keeps its Down and Forward.

**The readable lane signal.** `humanDrivesDummyLane` is its own field because `macroOverrideP1` stops meaning "recording" under a swap, where playback sets it. It is reported on every path, including while the re-arm gate holds the pair neutral, so the input viewer moves on the same step the routing changes.

**Publishing the input display.** The strip is published right after composition inside the admitted step, and never from `trainingPublishStatus`. That function also runs on non-simulating frames, after `trainingFinalizePendingStep` has zeroed the masks, so publishing there would fold in neutral history nobody pressed. The mask must be the composed one because policy, SOCD, the re-arm gate and relative facing all make it differ from the device.

### Macros and recording

`TrainingMacros` holds `kTrainingMacroSlotCount` slots of `kTrainingMacroFrameCapacity` frames, in memory only and cleared per generation. A `MacroFrame` is a logical mask plus the facing at capture.

| `MacroState` | Meaning |
|---|---|
| `PreRecord` | You drive the dummy lane to set up. Nothing is captured |
| `RecordArmed` | Clears the slot, then moves to `Recording` next step |
| `Recording` | Captures one frame per step |
| `RecordStopping`, `PlayStopping` | One neutral step, then `Idle` |
| `PlayArmed`, `Playing` | Replays onto the dummy lane |

Recording has two phases. `advance` sets `recordFrameRequested` and `recordCommitPending_`. After `TrainingInput::advance` has produced the real mask, `commitRecordedFrame` stores it. The macro therefore holds what the fighter actually received, after overrides and SOCD cleaning, not the raw device. A commit with the wrong serial, or a missing commit, stops recording rather than leaving a gap in the macro.

With `FacingPolicy::Relative`, `remapFacingRelative` swaps Left and Right if facing differs from capture.

Macro time counts only real steps. `MacroStepInput::paused` freezes the cursor during menu capture, the neutral step after an operation, and input re-arm, while keeping lane ownership so the human does not leak through.

Macros are not part of savestates. A load or reset stops the running macro, and `SetMacroAfterReset` can arm a play or record to start once it is idle again (`g_trainingMacroAutoStartPending`).

There are two extra sources of playback, both from triggers:

- `requestTriggeredSequence` plays a generated move of up to `kTriggeredSequenceCapacity` frames, not a recorded one.
- `requestBufferedReversalSequence` plays a move for the wakeup and block reversal path. It pauses at `bufferedReversalCommitIndex` until `releaseBufferedReversal` is called. The integration releases it on the last locked step, checking through `g_trainingWakeupReversal` that the fighter, action and remaining-lock arithmetic have not changed. Any mismatch calls `trainingCancelWakeupReversal`.

**Native reason for the armed steps.** `RecordArmed` and `PlayArmed` last exactly one neutral step, not until the player lets go. That step is the lane handover, and it gives a recording or macro opening on an already-held direction its own ring entry, because the game's direction ring only inserts when the value changes. In `PreRecord`, Play means leave and exits through the same neutral step as Stop, rather than starting playback.

**Generated moves.** A catalog sequence starts neutral to create a real history edge, and `leadingNeutralRedundant` drops that frame when the live input already provides the edge: no button held, and a direction different from the one being held. A crouch already holding Down keeps the neutral before a 22 motion, or that Down entry is too old for the matcher's window. The trigger code tests this against `g_trainingLastComposedInput`, because finalize has already cleared `g_trainingFinalInput` when the post-step observer runs.

**The release tail.** The commit is the last step that presses a button. Later steps may keep a direction held, which is what makes a crouching normal crouch when the parser reads the ring on the release frame.

### Savestates

`TrainingStateSlot` holds a full rollback image from `g_rb.snapshotTo`, the FPU state, the recovery RNG, and identity: generation, layout generation, `trainingCharacterFingerprint`, scene. There are `kTrainingStateSlotCount` slots. `g_trainingScratch` uses the same type.

- `trainingSlotCompatible` is the only gate for loading. All identity fields must match, so a state cannot load into a different pairing or layout.
- `trainingFinishRestoredImage` resets timeline machines, validates with `RollbackManager::LiveValidationPolicy::RawStructuralOnly`, re-applies config and ownership, validates again with `LocalTrainingSnapshot`, and checks invariants. It restores the FPU image last, because validation and sound work may disturb it.
- A failed load falls back to `trainingRestoreScratchOrFailClosed`.
- After a load, `g_trainingOperationNeutral` makes the next step neutral.

Anything that must survive a load has to be in a rollback region. Adding a new region is a rollback change, not a training change.

A restored image discards the timeline, so `trainingFinishRestoredImage` resets every machine describing it first, before validation, because a failed validation does not put that timeline back. Settings stay, because they are what the user selected. `preserveObservationCounters` keeps the recovery counters across loads, since they describe the session and the save, get hit, load practice loop would otherwise pin them near zero. `TrainingStateSlot::recoveryRng` is installed after that reset, so a load reproduces the delay sequence. It is training-local splitmix64, since a native RNG draw would perturb the simulation.

### Position reset

The game's round reset refills life, wipes every actor, clears projectile arrays and re-runs the intro, so a training reset never calls it. `trainingCaptureRoundCharacterState` captures one character image on the first frame both fighters report action 0, and `trainingResetPairToPreset` does the following, in this order:

1. `trainingDespawnTransientActors` unlinks spawned actors, then marks them free. Marking first would let the allocator reuse a slot the update list still walks. The partner slots are never freed, because they hold half of a transforming character. The shared auxiliary pool also holds stage actors, so only exact character and action pairs are freed, and they are not linked actors.
2. `trainingRestoreRoundCharacterActors` restores the actors, giving the teardown valid idle pointers even if the reset interrupted a hostile state.
3. `trainingClearComboReadout` calls the native combo teardown on both forms. Going to idle does not end a combo, so the hit count and scaling would survive, and the teardown is called instead of reimplemented because it owns global scaling state and the recovery lock.
4. `trainingRestoreRoundCharacterRegions` restores the command-history and other child regions, which the teardown also writes. Children can be shared by two forms, so they are stored once by address.
5. `trainingApplyPositionPreset` and `trainingApplyResetResourceState` apply placement and resources. Erasing an action also erases the EX payment and life settlement it started, so reset cancels those instead of returning busy.

`trainingAlternateFormActor` returns the player's second actor only when the engine would treat it as one. For most of the roster that slot is readable but empty, with no records, so callers must tolerate zero.

Overlapping boxes are ordinary engine input, because the game splits any intersection each frame. What matters is which box is on the left: it picks its push direction from the box centres, so a crossed pair would be driven further into the swapped arrangement, against the committed facing. `validatePositionPlan` rejects that, and the "separated" test means ordered, not disjoint. `cameraBias` is relative to the anchor midpoint, since the fighter anchor is the sprite origin and not the silhouette centre. If the game's own clamp disagrees with a validated plan, the reset fails and restores the scratch image and accepts no approximation.

### Guard and dummy policy

`trainingGuardDecision` gathers facts, and `chooseGuardDirections` in `src/training/TrainingDummy.cpp` decides. `chooseDummyPosture` then reduces guard, stance and recovery to one direction pair per axis.

- **Modes.** `GuardMode` values are stable. `GuardMode::AutoRecord` reads the attacker's currently active attack records and a lookahead over its upcoming frames (`trainingAccumulateActiveGuardRecords`, `trainingScanUpcomingGuardRecords`, bounded by `kTrainingGuardLookaheadTicks`), then holds the answer for `kTrainingGuardPostureHoldSteps`. `GuardMode::Auto` stays unqualified: `trainingGuardDecision` sets `autoPredictionQualified` false, and `guardModeReleaseQualified` requires it.
- **Capture records** are tracked separately (`captureAvoidPostureMask`). A capture lands only when its raw permission byte and the victim's posture byte share no bits, which is a different test from guard legality, so they must not be folded into the ordinary legal mask.
- **Facing.** The game judges guard by the defender's facing at the moment it reads input, and a turn can occur between composing input and that read. `trainingGuardAwayAtRead` re-points a guard's horizontal direction at the read itself, using `g_trainingGuardAwayRead`, which is only armed when the horizontal input came from guard.
- **Knockdown roll.** Holding plain back during a terminal knockdown can spell the native roll command. `trainingGuardAvoidTerminalRoll` tests the direction ring with `terminalRecoveryWouldMatch` and holds down-back instead.
- **Recovery yields guard.** A blinking direction can read as a tech, and `TrainingRecoveryController` owns the inputs while it emits one. `chooseDummyPosture` gives it both axes, `dummyExclusive` makes `TrainingInput::advance` reject anything less, and `policyStandsDownForRecovery` stops the policy fighting a human's tech window while recording.
- **Timing.** `guardTimingAdvance` in `src/training/TrainingGuardTiming.h` decides when guard is allowed, separately from how: `Always`, `AfterBeingHit` with a window that counts only neutral steps, or `UntilFirstBlock`, which re-arms after a neutral stretch. `trainingRecordContact` latches `g_trainingGuardTimingPendingHit` and `g_trainingGuardTimingPendingBlock` during the step, and the next compose consumes them. If timing is not armed, the effective mode is `GuardMode::Off`.

**Native guard and recovery facts.** The guard initiator switches on the low three bits of the fighter's posture byte: 1 needs the stand permission, 2 crouch, 4 air, and any other value requests no guard at all. This is why `PostureClass::NotGuardable` is a separate answer from grounded. Native manual recovery needs several things at once: an eligible reaction action, the authored per-frame permission bit, a clear per-slot lock, the posture gate, and the command matcher accepting tap, neutral, tap inside its window. The direction ring advances only when the masked value changes, which has two consequences:

- A held direction never produces a recovery, and only makes the dummy block.
- If the dummy already holds the tap direction, the first tap adds no ring entry and the attempt vanishes with no error. `RecoveryState::EmitPreNeutral` exists to prevent that.

The matcher also rewinds on a mismatch, so a completed pattern can fire later with no new input. `RecoveryVerdict::BufferedObserved` keeps that case apart from both a controller success and the game's automatic recovery. When reading the recovery panel, check the stand-down bits first: `RecoveryStandDownMenuCaptured` means that frame-stepping through a knockdown with the menu open can never produce a recovery.

**Axis ownership.** `chooseDummyPosture` resolves everything into directions plus owned axes before `cleanSocd` runs, because policies that merely contributed bits would fight inside the cleaner: a held toward would erase a requested away under neutral SOCD. Recovery takes both axes, and its neutral steps are true neutral, because the native command matcher compares a pattern entry with the ring entry by exact equality over all four direction bits. A guard's back held under a recovery tap would record an entry that matches no pattern. Button ownership is all four or none for the same kind of reason: the native throw escape requires the button ring head to be exactly C, so a human's held D left in the word makes the escape silently never fire. `DummyTech::HoldGuard` stays declared only so a stale press is rejected. A held direction adds no ring entry, since the ring records only changes, so it can never tech.

**What the native guard reads.** The game's guard parser reads the ring head and the defender's posture nibble (the low three bits of the fighter's posture byte), which picks action 18 (stand), 19 (crouch) or 20 (air). Other nibble values request no guard, which is why `PostureClass::NotGuardable` is its own answer and not a fold into grounded. In the air, back, up-back and down-back all request action 20, so Down buys nothing there, and a Down still held on the landing frame becomes a crouch at the worst moment. `GuardMode::AlwaysCrouch`, `GuardMode::AutoHeuristic` and `GuardMode::AutoRecord` therefore all drop it while airborne. `PostureClass::Unreadable` comes only from a failed pointer and never gets a guessed posture.

The modes make these choices for reasons worth keeping:

- `GuardMode::AlwaysAir` holds one constant up-back. That single value guards in both postures, it is also the back-jump request, and a constant emission produces no ring transition on the take-off and landing frames the matcher is reading. Posture is sampled a step before the simulation, so a value that switched at the boundary would move the ring head on exactly those frames.
- `GuardMode::AutoRecord` deliberately has no Up, because Up re-triggers the back jump on every landing and the dummy would never come down.
- Crouch is the default in `GuardMode::AutoHeuristic` and `GuardMode::AutoRecord`, since it blocks everything stand guard does except grounded overheads, and also covers lows.
- `away` is held unconditionally in both, so late evidence can cost only the refinement and never the block. Gating on a threat signal would create a one-step-late guard for no benefit.
- `chooseGuardDirections` treats a tech result as grounded. For most characters the tech (33, or its wall sibling 34) is an airborne hop that ends inside the landing step, so the entry the first free frame is judged on was composed during the hop. Nothing in the hop can be guarded, so its hold only ever decides that first grounded frame.

**Reading attack records.** `GuardMode::AutoRecord` follows the same addressing the native hit resolver uses, validating every pointer and range. Refused evidence and an idle attacker both fall back to the crouch default, and `recordEvidenceMissing` tells them apart. Four native data facts shape the reader:

- A frame's record count is not always a count: 0 clears the attacker's collision flags, 254 means no collision and 255 is a one-record saved-state probe.
- A frame's next link makes the list a graph: 0xFFFE terminates, 0xFFFF advances, anything else jumps. Walking it as an array reads the wrong frames for every move with a landing branch, so `trainingScanUpcomingGuardRecords` follows the link.
- A duration of 0xFFFF is a conditional hold until something happens, usually landing, and not 65535 ticks. The walk steps through it at a nominal cost of `kTrainingGuardSentinelNominalTicks`, for the current frame's remaining hold as well.
- Grab records (type 15) use an inverted test, and types 13 and 14 are attack-versus-attack and answer no posture question.

Simultaneous records are intersected, never merged, because stand-only together with crouch-only is a real mixup and not "either". `recordMixupUnblockable` reports that case while the dummy holds the crouch default. `legalPostureMask` is published because "stood up and ate a low" and "correctly blocked a stand-only hit" otherwise look identical.

Input composed now is worn on the next engine step, so `trainingAccumulateActiveGuardRecords` folds in the upcoming frame even when the current frame already has records. If the two disagree, the upcoming requirement wins, because intersecting them to empty would block one frame late. `kTrainingGuardLookaheadTicks` is sized so that a long startup still gives a crouching dummy time to stand. `kTrainingGuardPostureHoldSteps` exists because evidence disappears briefly when a landing redirects into a new action, which is when the follow-up lands. An absent alternate-form actor contributes nothing; only a readable actor with a malformed layout refuses. Projectiles and helpers are not scanned, since a spawned actor is a full copy with its own posture byte, so that blind spot is real.

`[training-guard]` lines (the state-log switch) show `now`, `ahead` and `chose` per attacking actor. A step where `now` and `ahead` differ is where a frame-late block happens, and `TrainingGuardLookaheadStop` says why a walk ended.

**Guard timing details.** In `AfterBeingHit`, a hit always re-arms the window, and a block refreshes it only while already armed, because an unarmed dummy cannot have blocked. Stun and knockdown steps do not count, so a combo ending in a sweep still has guard on wakeup. The contact flags and the neutral test follow the fighter the settings act on, so during a recording they count your own fighter.

**What the other modes hold.** `chooseGuardDirections` needs no timers because every mode is a held direction, and the game's guard parser does the rest. The game's guard test switches on the low three bits of the fighter's posture byte: 1 needs the stand permission bit and requests guard action 18, 2 needs crouch and requests 19, 4 needs air and requests 20, and any other value requests no guard at all. `classifyPostureNibble` keeps that last case as `PostureClass::NotGuardable` rather than folding it into grounded, and `PostureClass::Unreadable` is a failed read, never an observed value. `postureClassAirborne` and `postureClassGrounded` are therefore not complements. A fixed mode such as `GuardMode::AlwaysCrouch` tests `postureClassAirborne`, so an unreadable posture keeps the grounded answer instead of guessing air.

- `GuardMode::AutoHeuristic` chooses from posture alone: a grounded defender crouches unless every readable attacker is airborne. It never reads the attack record, so it is wrong for grounded overheads, throws and projectiles, and it decides on end-of-previous-step state. It is a separate enumerator so it cannot be mistaken for `GuardMode::Auto`.
- `GuardMode::AutoRecord` intersects the legal posture sets of the active records and never unions them, because stand-only plus crouch-only is a mixup the defender cannot block. An empty intersection sets `recordMixupUnblockable`. `away` is held in every posture, so only the secondary axis moves.
- `GuardMode::AlwaysAir` holds away plus Up in every posture and exists because the other modes own both axes, which suppresses `DummyStance::Jump`, and a dummy that never leaves the ground never reaches guard action 20. The game accepts that single direction as stand block on the ground and air block in the air, and up-back is also the jump request. Keep it constant: posture is sampled one step stale, and a value that changed on take-off or landing would put a ring transition on exactly those frames. A constant hold creates no transition, so it cannot match or destroy a native pattern.

The `validCommandPayload` bound on `SetGuardMode` is a range test only. `GuardMode::Auto` stays inside it so that `guardModeReleaseQualified` is the thing that refuses it. A new mode must widen that bound, or it fails at ingress and shows up as a dead menu entry.

Stance is also only held input: crouch holds Down and jump holds Up, and the game re-triggers a jump on ground contact while Up stays held. Nothing in the dummy policy writes a fighter action field.

### Native recovery

`TrainingRecoveryController` in `src/training/TrainingRecovery.h` makes the dummy tech by injecting real input. It writes no action field, and `g_trainingRecovery` and `g_trainingRecoveryP1` are one instance per side.

**The native rule.** A manual tech is the conjunction of an eligible reaction action, the authored per-animation-frame permission bit, a clear per-slot lock, an admitting posture, and the game's command matcher accepting tap, neutral, tap inside its window. The direction ring gains an entry only when the masked direction changes, so a held direction produces no entry, no matcher call and no tech. This is why `DummyTech::HoldGuard` is refused by `dummyTechReleaseQualified`: holding a direction only ever made the dummy block. `DummyTech` values stay frozen.

| `RecoveryBranch` | Pattern | Window | Result actions |
|---|---|---|---|
| `Air22` | Down, neutral, Down | `kRecoveryWindowAir` | 31 |
| `Ground88` | Up, neutral, Up | `kRecoveryWindowGround` | 33 |
| `Wall66` | Forward, neutral, Forward | `kRecoveryWindowGround` | 33, 34 |
| `TerminalForward66` | Forward, neutral, Forward | `kRecoveryWindowTerminal` | 13 |
| `TerminalBack44` | Back, neutral, Back | `kRecoveryWindowTerminal` | 14 |

Native facts behind the table and its gates:

- The wall tech lands on 34 when the prior action is 37 and the animation frame is 0, so a success set without 34 would score that tech as a miss.
- Only the air, ground and wall branches read the manual lock (`recoveryBranchConsultsManualLock`). The terminal code never reads it. The lock clears only on teardown, so cancelling a terminal attempt because of a combo's lock would make the option silently never fire.
- The manual block runs only for posture values 1, 2 and 4 (`recoveryPostureAdmitsManual`).
- The KO latch blocks every transition commit, including 31 and 33.
- While the command child's round-transition flag is set, the game records every direction as zero, so a tap during it is recorded as neutral. This is reported as a stand-down, not a miss.
- `ThrowEscape` is a frozen, inert id, kept so a stale wrapper gets an explicit refusal rather than a reinterpreted one. Its bit is never armable.
- The public names of the reaction actions are unresolved: control flow proves only set membership. The UI shows the branch name and numeric id, never an invented move name.

**Arming.** Attempts start on a rising edge of `recoveryArmableMask`. Level-triggering would re-arm through a whole knockdown and overrun the 16-entry matcher scan (`kRecoveryRingScanCap`). Two extra sources cover what an edge cannot see:

- A change of `hitEpoch`, which counts hit contacts. A later hit of a string can leave action, permission bit and lock all unchanged, so no edge ever comes. Zero means unknown and is never an edge.
- `kRecoveryRetryGapSteps` after a `Cancelled` or `MissedExpired` attempt while the branch stays armable. Eight steps lets the old pattern age out of the scan, since an attempt writes at most four entries, and still lands inside a realistic window.

`recoveryEligibleMask` is the full conjunction and `recoveryArmableMask` omits the terminal branches' ring-head term, which the controller creates itself by holding the direction. Both are published, because a terminal row drawn from the eligible mask alone reads as unavailable on the very frames the controller would arm.

**Emission.** An attempt spans distinct admitted steps. One admitted step is one native simulation iteration and one ring frame, so paused and frame-held steps add nothing. `EmitPreNeutral` is required unless the ring head is already zero: if the dummy is holding the tap direction, Tap1 makes no ring entry and the attempt evaporates with no error.

`recoveryAwaitMask` then differs by branch. The manual matchers look for a subsequence, so those branches hold true neutral. That is also the only full protection against a buffered jump from the 88 pattern, because a matcher that rewinds on mismatch can still match Up then neutral until the hold count retires it. The terminal gate reads the ring head directly, so those branches keep holding the direction, because a neutral step would put a zero there.

Matcher modes 0 and 1 rewind on mismatch, so a finished pattern stays matchable for its whole window and can fire a tech with no new input. `RecoveryState::ObservingBuffer` watches for that, dated from the last tap (`patternLiveUntilStep`) rather than from when emission stopped, because a lock appearing mid-pattern parks the attempt early while its taps are still matchable.

**Reading a refusal.** `RecoveryVerdict` separates three cases that look alike: `Succeeded` while emitting, `BufferedObserved` from the leftover pattern, and `AutomaticObserved` when no taps were sent. `RecoveryCounters::commitBlocked` counts the matcher requesting a transition while the KO latch refuses to commit it, which is a refusal downstream of the matcher. The latched `tapsEmitted` tells opposite bugs apart: `MissedExpired` with two taps means the game refused a real pattern, and with zero it means the attempt never emitted one.

When the eligible bits are dark, read `recoveryStandDownMask` first. It is the single source for both gates that zero the masks, through `kRecoveryStandDownAvailabilityMask` and `kRecoveryStandDownCommonGateMask`. It is published unconditionally, because `RecoveryStandDownNotReadable` is exactly the case in which `recoveryReadoutValid` is clear.

- Macro playback stands the controller down. `guardDuringPlayback` yields per axis, and owning half of an exact-equality pattern produces a mask that matches nothing.
- `RecoveryStandDownMenuCaptured` catches testers: a frame-step credit still admits a step while the menu holds the game, so stepping through a knockdown with the panel open can never produce a tech.
- The raw wall-contact flags are a sanity term only. The matcher never reads them; action 37 is the engine's own wall proxy, and a stale 37 with cleared flags is why they are published.

**Determinism.** `ManualRecoveryMode::RandomDelay` and `TerminalRecoveryChoice::Random` draw from `recoverySplitMix64`, never the game's RNG, because the simulation consumes that one. `TrainingStateSlot` stores `g_trainingRecovery`'s state as `recoveryRng`, so a load reproduces the delay sequence. A load or scratch restore calls `resetTimeline`, not `reset`. The in-flight attempt must go, because its ring was replaced, but the counters survive, since the practice loop is save, get hit, load, and a full reset would pin the totals near zero. `kRecoveryLateDelaySteps` is a comfort value with no native basis: the real limit is the authored permission window, which nothing reads.

**The release gate.** `nativeRecoveryReleaseQualified` turns every non-Off mode into Off until `qualifyRecoveryEvaluator` has run, and its default arm returns the gate so an appended mode cannot ship unqualified. `recoveryEvaluatorReasonFor` keeps the failures distinct: `NotAttempted` (the seam check or hook transaction refused first, and no check has run), `AddressUnresolved` (a constant address would be wrong on another build, so symbols resolve through the active profile), and `PrologueMismatch` (the address resolved but the bytes differ). A pass proves the expected code is loaded. It says nothing about whether a tech works in play.

### Manual recovery

`TrainingRecoveryController` is pure and has no route to game memory. It takes a `RecoveryObservation` and returns logical input. It never writes an action id or a lock, so it cannot force a recovery. Automatic native recovery is only classified (`AutomaticObserved`), never suppressed or scheduled.

These are native rules. A manual recovery needs an eligible reaction action, the authored permission bit, a clear per-slot lock, the posture gate, and the command matcher accepting tap, neutral, tap inside its window. A held direction satisfies none of them, because the direction ring records only value changes and the matcher runs only when the head's hold count is zero. The matcher compares exact words and scans at most 16 ring entries, which explains the controller's shape:

- It owns both axes and is exclusive with playback, because half a pattern matches nothing. Changing direction every step while a pattern is live can also push the taps past the scan.
- It emits four steps. The pre-neutral matters when guard or stance already holds the tap direction, since tap 1 would otherwise create no ring entry and the attempt would vanish with no error. It is skipped when the head is already neutral.
- Facing and lock are not cancel terms. The ring stores facing-normalised entries, so a mid-sequence flip leaves earlier taps valid. A lock appearing with taps already emitted parks the attempt in `ObservingBuffer`, because the game clears the lock on combo teardown and the matcher may still fire.
- `recoveryBranchConsultsManualLock` is false for the terminal branches, because the native terminal arm never reads that lock.
- It holds the axes for only `kRecoveryAwaitCapSteps` after the final tap. Holding for the full window would leave the dummy unable to guard.
- A wall tech tests the reaction action only. The native per-tick loop zeroes the wall-contact flags and collision re-sets them only while touching the wall, so they read clear for most of the reaction.

`recoveryArmableMask` and `recoveryEligibleMask` are both published. The terminal branches require the ring head to already be the tap direction, and the controller creates that head itself, so the eligible bit stays dark on the frames it would arm. A panel showing only eligibility reports "no" for a branch about to fire.

Arming is on an edge, because level-triggering would restart through a long knockdown and blow the ring scan. Two extra arms cover boundaries an edge cannot see: a new hit epoch, and a retry `kRecoveryRetryGapSteps` after a miss or cancel. An attempt may claim a result once `tapsEmitted` is above zero, because the matcher can accept before the last tap when a held direction already supplied the first. A `BufferedObserved` verdict is deliberately ambiguous, since a completed pattern stays matchable for the whole window. Verdict and tap count are latched together: `MissedExpired` after two taps means the game refused a real pattern, and after zero means none was emitted.

`RecoveryBranch::ThrowEscape` keeps only its numeric id and is always inert.

**Stand-downs are not misses.** `recoveryStandDownMask` is published even when the observation is unreadable, because `RecoveryStandDownNotReadable` is the bit that explains the cleared readout. Under COM ownership or direction suppression an injected tap never reaches the ring, so those read "not eligible", never "missed".

**Blink protection while recording.** Your own fighter's lane is ours during a recording, and a guard hold that disarms and re-arms writes hold, neutral, hold into the ring, which is the native air-tech pattern. `policyStandsDownForRecovery` therefore gives the whole armable window to the recovery controller, including a configured delay that the posture would otherwise beat. The window covers only air hitstun, knockdowns and wall reactions, where the fighter cannot guard anyway. The dummy's own lane is not stood down this way, because meaty-after-tech needs its hold present on the first actionable frame.

### Contact policies

`src/training/TrainingContactPolicies.h` covers Faultless, Parry, Critical and Reduce, packed in `TrainingContactPolicySet` and set by `SetContactPolicy` through `applyContactPolicyAssignment`. Each detour runs the native function, then changes the outcome only when `trainingContactPolicyActive` is true: training live, not failed, and on the simulation thread.

| Detour | Policy effect |
|---|---|
| `hkTrainingFaultlessDetector` | Replaces the mode with `faultlessFamilyModeForActor`, or 0 |
| `hkTrainingGuardActionRequest` | Parry: reproduces the matcher's two writes (`kParryMatcherActionRequest`, `kParryMatcherPendingFlag`) or undoes them |
| `hkTrainingReduceDetector` | `reduceVerdictForPolicy`, per victim slot, mode 1 only. Mode 0 is a different check and passes through |
| `hkTrainingCriticalScaler` plus `hkRngDraw` | Forces the sample the scaler draws |

Faultless and Parry detours act only on the fighter in P2's slot, whichever role the swap has given it.

**Critical keeps the RNG in step.** The native draw always happens. `trainingCriticalRollOverride` swaps only the returned sample (`criticalSampleForOverride`), so the seed advances identically under every policy. The guard-crush latch is read before the scaler runs because the scaler consumes it, and `NeverAny` clears it beforehand, because a finished scale cannot be undone.

`resolveContactPrecedence` models the native order: parry first, then Faultless. `TrainingContactPolicySet::allNative` means no change from the game's behavior.

The tables here match both supported builds, so this module has no per-build branching; only addresses differ, and those belong to the runtime profile. The rule behind all of it is to override the native decision at its seam and never reimplement the consequences. Chip, gauge, meter, hitstop and the red flash all still come from the native resolver.

Native facts encoded here:

- **Critical.** The divisor comes from the defender's combo count plus one, the hit about to land, which gives chances of 1/32, 1/8 and 1/4 (`criticalDivisorForIncomingHit`). A guard-crush latch separately doubles the hit, so forcing the roll on top is the native route to about x4. `TrainingCriticalPolicy::AlwaysSingle` therefore declines the roll when the latch is set, and `TrainingCriticalPolicy::ForceRandomRoll` exists to study the stacking.
- **Faultless.** The detector returns a character family mode the resolver consumes, not a boolean, so `faultlessFamilyModeForActor` reproduces the table. It is broader in meaning than the resolver's whitelist: the resolver only calls it for `faultlessSupportedActor` ids. The meter gate is strictly greater than 6 fractional units, then 6 are spent. The guarded path later grants 1, so a block usually nets -5, which is a native observation and not arithmetic done here.
- **Parry.** `parrySupportedActor` includes id 0 (Saori) and the SE alternate Rapid Ayaka (31). `AlwaysWhenLegal` overrides only the input matcher, and the other gates (`parryPostureEligible`, `parryAttackerExcluded`) still hold because the resolver checks them first.
- **Reduce.** The native detector counts the victim's recent button edges: at least 4 inside 10 frames returns 2 (70% damage), inside 25 frames returns 1 (90%), otherwise 0. The resolver keeps its own arithmetic and floor of one. Mode 0 of the same function is the struggle-escape check, which is why the detour passes it through.

`t_trainingCriticalContext` confines the RNG override to the draw the scaler makes, and never AI, effects or other consumers of the same stream. It is thread-local because only the game thread may override.

### Resources

Life, guard gauge and EX all follow the same transaction:

1. A pure plan from `src/training/TrainingTypes.h` (`planLifeRefill`, `planGuardGaugeRefill`, `planExRefill`, `planLifeSet`, `planExSet`) takes an image of the record and returns values or a rejection.
2. Every target is planned before any is written.
3. The integration writes, reads back with a helper such as `trainingLifeComponentsMatch`, and on a life mismatch undoes what it wrote, calling `trainingFailMutation` only if the undo fails. Guard gauge and EX have no undo: a write that does not read back calls `trainingFailMutation` directly.

Field order matters. Life writes `current` first so a fault cannot leave a lethal intermediate value. `planLifeMax` derives the maximum from the active config, using `kLifeUnitsPerLevel`.

`TrainingRefillTarget` cannot express slots 2 and 3. Those stay untouched because the native round code would dereference a null partner pointer if they took a fatal path.

Automatic refills come from `trainingConsumeResourcePolicies`, which feeds `ResourceTriggerSignals` to `resolvePolicyFire`:

| Signal | Source |
|---|---|
| `neutralReady` | `NeutralTrigger`, `kTrainingNeutralHoldSteps` of clean `evaluateFighterNeutral` on both fighters |
| `exactIdleReady` | `IdleStateTrigger`, `kTrainingExactIdleHoldSteps` of free-movement actions |
| `comboEpisodeClosed` | `ComboEpisodeMachine`, `kTrainingComboSettleSteps` after damage settles |
| `recoveryExited` | `RecoveryExitTrigger` |

Health uses `resolveHealthPolicyFire`, which under `ResourceTrigger::WhenNeutral` ignores the neutral signal and fires only on exact idle or an accepted reset. `resolvePolicyFire` accepts either signal for guard gauge and EX. Neutral masks are bit sets (`TrainingNeutralReason`) reported in status, so the overlay can show why a fighter does not count as neutral.

**Native records.** The per-slot combat record (`kTrainingCombatRecordBase`, 44 bytes per slot) holds combo participation bits, the guard-crush latch, the guard gauge, the combo count and the recovery lock. `kTrainingComboHitsOffset` is the value the game's own proration reads, so the readout samples it instead of counting: chip enters the damage function but is not a combo hit. A gauge refill leaves the crush latch alone, since the queued penalty is a separate field. The recovery lock is read-only, because clearing it would manufacture a tech eligibility the game never granted. Combo accumulators are indexed by the victim's owner side and match records by the attacker's, so one reads "taken" and the other "dealt". The damage readout's per-hit facts (`g_trainingDamage`) exist nowhere in game memory; everything else is read live, because those pools sit in savestate regions and a cached copy would disagree after a load. The nominal damage is reported, since the endless-round floor clamps a lethal hit's life change to zero. `trainingClearLastDamageEvent` sets `kTrainingNoDamageDescription` as the victim slot, so a cleared description cannot render as a real hit of zero.

**Native flag not used for refills.** `kNativeFatalHitDisableAddr` is the flag the game's own F4 toggles, and it is not a refill. With it set, settled damage evaporates, every owner gains an EX fraction per frame and several resource checks act as if stocks were available. `trainingClearNativeDeveloperEdges` repairs it to zero at every gate call and `trainingInvariantsHold` asserts that, because the toggle itself is outside the native debug-config gate. If it were ever nonzero no resource observation would mean anything, so the neutral predicate refuses as well. Refills are explicit one-shot actions on verified fields: they do not coalesce, and a savestate load reverts them on purpose.

**EX in flight.** An EX activation is granted first and debited later, one stock at a time, during the spend animation. Refilling or setting stocks in that window would make a granted super free, so `planExRefill` returns `RefillVerdict::Busy`, which is retryable. Stocks are written before the fraction: an interrupted write reads as full stocks with a stale fraction, which the next native meter call corrects, while the reverse order would discard progress. Spend state, the timer and the pending lanes are never written. A life set can lower a value, so `planLifeSet` plans the write order and every prefix of it keeps effective life above zero.

**Neutral means two narrow things.** `evaluateFighterNeutral` accepts only the free-movement actions on the two action fields, and matches what the game's round reset and idle handler install on every other clause. Neither half proves a fighter can act next frame. Its tick runs once per admitted step, guarded by the step serial, because its seam sits before `trainingAdmitPausedFrame`: otherwise settle counters would advance in wall-clock time during a hold and a paused menu could close a combo. The tick therefore observes the step admitted previously, which is the one that produced the state being judged. Each resource's fire is a separate transaction, and a rejected one neither grants nor blocks another.

**EX pinning.** Charging pins `g_trainingExRestoreTarget` to the level just charged, so automatic restore does not overrule it. The pin survives resets and loads, because it is a preference about what "restore" means and not part of the deterministic image. A new generation clears it.

Holding F4 shuts out F5 (`chargeHeld` requires `!refillHeld`), because both write one meter and would fight on alternating frames. A held-key refill result is logged, not treated as failure: the helpers answer busy while an EX activation is being debited, and a transient refusal must not become a sticky error. `trainingClearNativeDeveloperEdges` runs on frames that never simulate, so the native F4 edge only latches a request. `trainingConsumeRefillRequests` consumes it inside the owned step, after the hotkey table has settled any load and before the postcondition, so a failed refill hits the same invariant check and scratch restore as everything else. `trainingConsumeResourcePolicies` has the same position for the same reason.

**Damage readout.** `hkTrainingDamage` judges admission before the endless-round floor rewrites pending damage, or every lethal hit would look like an anomaly. The native body returns its completion field, so a zero result means the hit was admitted. It cannot see a table swap mid-call, so the pending-damage delta is the independent check. An admitted hit with the wrong delta increments `anomalySerial` and a sticky error and carries on, because nothing in the floor depends on this readout. The owner slot comes from the victim's slot byte, and the target table entries need not repeat that index in their own byte, since normal table swaps break that equality natively. This code runs inside native collision, so it must not allocate, lock or open a file.

**Why neutral is an allow-list.** The action space is only partly mapped, so a deny-list would call an unmapped action neutral. `trainingFreeMovementAction` accepts exactly idle (0), walks (26, 27) and crouch (28), each observed in the running game as able to act on the next frame, and `evaluateFighterNeutral` tests the dispatched action field as well as the primary one, because the primary field carries ids the dispatcher never switches on, such as airdash. The two fields need not be equal. The other clauses match what the game's round reset installs and its idle handler maintains. Wall contact is deliberately not a clause, because standing idle in the corner is neutral. The predicate is sound but not complete: "not neutral" means "not proven settled", never "cannot act", so labels must not say "actionable". The ids were observed on one character, and no per-character survey backs them.

`trainingReactionOrRecoveryAction` is the one deny-set, and it is safe because over-inclusion only delays a refill. Its limits are chosen with care. Guard 18 to 20 stay in, because a blockstring is not a settled episode. Everything from 200 up stays in because that is the game's own test for throw reception. Dash 13 and backdash 14 stay out: they have their own handler, and listing them would hold a combo episode open through every dash and fire `recoveryExited` on dash to idle.

`evaluateRoundStartNeutral` tests action 0 exactly, and must not share the free-movement list, because it asks whether one step preserved the restored idle image. A fighter that comes out of a reset walking fails it. Its result is split on purpose. `trainingRoundStartNeutralEscalatingBits` covers round result, COM and frame pointer, and a violation there calls `trainingFailMutation`. Any other cleared bit is only reported, because undoing a validated reset over an informational detail would be worse.

`ComboEpisodeMachine` needs three independent signals, the game's combo counter, its settlement idleness and the admitted damage serial, because the action map is incomplete. Unsettled damage counts because refilling while pending damage is still moving would race the game's own subtraction. `recoveryExited` fires on leaving the reaction family whatever caused it, including the game's automatic recovery. The policy only observes.

**Life.** Current at maximum with pending, reserve and timer all zero is a fixed point of the game's settlement, which is why a refill writes all four fields, not just current. Nothing drifts until the next hit. A `planLifeSet` can lower life, so its order is part of the plan: `planLifeFixedPoint` raises current first, clears the other three, and only then lowers current, so every prefix stays non-lethal. `planLifeReset` runs with a minimum of zero, because the endless-round floor can leave a legitimate zero, and `SetLifeValue` keeps its minimum of one. `kLifeLevelClamp` is a sanity check on the config read, not a native bound.

**EX.** The game grants an activation first and debits it afterward, one stock at a time, so zeroing a pending lane in that window makes a granted super free. `ExRefillPlan` and `ExSetPlan` have no field for state, timer or lanes, so they cannot express that write. `RefillVerdict::Busy` is checked before range validation and outranks `RefillVerdict::Corrupt`, because a super being paid for is a retryable answer, not a sign of an unmapped writer. `RefillVerdict::Invalid` blames the request, and `Corrupt` blames the record. Nine stocks with a zero fraction is what the game converges to, since any mutating call at nine forces the fraction to zero, so `planExSet` rejects nine with a nonzero fraction instead of coercing it. Do not call that state infinite meter, because it cannot pay the guarded fractional debit.

A position reset is the one case that does clear the lanes. It erases the action that owned any in-flight payment, so the integration zeroes state, timer and lanes to their round-idle values, and `planExReset` is separate so the ordinary refill keeps its `Busy` refusal. `planExFractionCharge` wraps nine stocks to an empty meter, so holding the key keeps charging. `ExRestoreTarget` is why charging to two stocks survives the next automatic restore.

**Guard gauge.** The game regenerates it by 1 every eight match ticks up to 100, so any lower value set by `planGuardGaugeSet` walks back to full in about 13 seconds. That is not a failed write. An observation outside 0 to `kGuardGaugeFull` means an unmodelled writer, so the plan refuses rather than clamps.

**The native F4 flag** does not heal. It makes settled damage evaporate, gives every owner free EX each tick, and makes resource checks pass as though stocks existed, so any resource observation under it is worthless. `trainingClearNativeDeveloperEdges` forces it to zero every time, and `NeutralFatalDisableFlag` reports it. `SetHpRegen` is the retired command for this flag. Its number stays reserved and rejected, because struct-size negotiation can detect a struct that grew but not a command kind whose meaning changed.

`ResetRoundStartNeutral` reproduces the values the round reset installs through the game's own transition request and the `PositionPreset::RoundStart` preset. It does not call the native round reset, which restarts the round in all but name.

### Reset positions

`src/training/TrainingPositions.cpp` computes and validates a full layout before anything is written. `RoundStart` reproduces the game's own fixed anchors (P1 at camera-32, P2 at camera+96), which `ResetRoundStartNeutral` depends on. The other four derive separation from the live collision boxes so the pair lands touching, which makes `Center` and `VeryClose` the same layout. Both enum values remain because they are ABI.

- **World order.** `closeAnchorSeparation` takes bounds in world order, not player order, and `LeftCorner` is the preset where those differ. Facing is set before the boxes are read, because the box is facing-specific.
- **The facing floor.** Touching edges are not enough. The native turn rule compares collision-box centres, and a facing-1 box is the mirror of the facing-0 box. Placed too close, the centres order opposite to the origins, both fighters turn, the turn re-mirrors the boxes, and it never converges: both alternate the turn action forever while one teleports. `centreOrderedSeparationFloor` prevents this. It costs nothing for symmetric boxes.
- **Overlap is legal.** The game's round start applies no geometry check, and its separation pass pushes intersecting boxes apart every frame, so rejecting overlap would fail closed on a layout the game makes itself. That pass picks direction from which box centre is left, so `FightersCrossed` is the real error: a crossed pair gets driven further into the swap.

### Triggers and frame advantage

`TrainingTriggerController` and `FrameAdvantageTracker` in `src/training/TrainingCombatTools.h` run after each step from `trainingAdvanceCombatToolsPostStep`. They consume a `CombatFrameSample` plus contact events recorded by detours such as `hkTrainingGuardContact` and `hkTrainingHitContact`.

A trigger maps a `TrainingTriggerEvent` to one `TrainingTriggerConfig`: a recording slot or a catalog move, never both (`trainingTriggerConfigValid`), plus chance and delay. Decisions go to a bounded journal (`kTrainingTriggerJournalCapacity`). Entries overflow by being dropped and counted, because the game thread never waits.

`prebufferEvent` limits buffered reversals to `OnWakeup`, `AfterBlock` and `AfterHitstun`. Techs are excluded because a motion in the ring could satisfy a native tech pattern by accident.

**The gate.** A native response is admitted by two terms, and `trainingNativeResponseReady` needs both. The basic-action bit must be set in the authored permission byte. For the reaction family only (`combatActionIsNativeReactionFamily`), bits 0x04 and 0x08 must also be clear. Ground hitstun 21 to 23 is not in that family. During a knockdown a frame can expose the basic bit while the recovery bits are still up, and the game refuses the input. `trainingCombinedAuthoredPermission` is the effective byte, because in the alternate-form state the evaluator ORs in that actor's permission. Action 36, the collision and fall route, commits a transition only on a lifecycle value the command evaluator never writes and is outside the reaction family, so `combatActionCannotCommitCommand` lets it arm a wakeup but never fire one. A response starts once per episode, so firing there would spend it silently.

**Why reversals are prepared in advance.** Button parsing happens only on a tap's release frame or the second held frame, so a press can never produce a command on the frame it is made. Observing readiness after a step and composing for the next costs another frame. A frame-1 reversal must therefore know the actionable frame beforehand, and the data allows it: each frame descriptor gives posture, permission, duration and next link, so future permissions are readable now. `trainingTicksUntilResponseReady` walks that chain, and `trainingStepsUntilDummyCanAct` prefers it over `trainingWakeupLockedStepsRemaining`, which walks to the terminal. The two differ: the gate opens on recovery frames before a knockdown ends, and aiming at the terminal lands one tick late, with the motion's held direction rendering as its own action first. Each walker fails closed on anything that could redirect the chain, such as a queued forced action, hitstop or a landing branch, and on an alternate form's second chain. `g_trainingSuffixRefusal` and `g_trainingStunWalkRefusal` name the clause.

**Stun is readable.** Blockstun and ground hitstun are not authored. On entry the native handlers write five pose durations into the frame table from the guard class, totalling 16, 20 or 23 for guard and 18, 22 or 25 for hit, so `trainingStunRemainingSteps` reads what will play. It is a separate walk because the transition flag is routinely set throughout a blockstun, which the wakeup walk treats as a refusal. Air guard holds on a sentinel and refuses. Guard accepts a mid-stun transition (the guard cancel), while hitstun holds a command until the terminal. During guard, permission bit 0x04 is the guard-cancel window, not a recovery permit.

**Motion and guard share the ring.** The native guard reads only the ring head at contact. The command matcher walks back from the head, skips entries that do not match, and charges their dwell, not their count, against the pattern window. A motion can therefore live behind the head while the head keeps the guard direction, and `trainingSeedMotionIntoRing` writes it there with one tick of dwell per entry, then the commit button is played. This never touches the head, and the character's own evaluator still decides meter and legality. A seed has two hazards, and both are handled with a barrier: a neutral entry of `kTrainingSeedBarrierDwell`, larger than any window the game uses, so the matcher spends its budget crossing that slot.

- The guard's own down-back can be read as part of a longer command, so 236 becomes 41236. The seed writes the barrier behind its oldest entry.
- The button may never come, for example when a blockstring's next hit replaces the episode. The motion would stay reachable by any later button, so `trainingRevokeSeededMotion` stamps the barrier just behind the head.

The buffered path is similar. `trainingPadReversalSequenceWithGuard` puts guard steps back between the motion and the button, so the dummy is off guard only for the walk. `kTrainingCommandRingWindowFrames` is a spacing budget for that pad, not the game's window, which is an immediate inside each character's evaluator. A knockdown has no such cost, since a knocked-down fighter cannot guard.

**Episodes.** A new contact is a new episode, and any delayed response from the previous one is discarded, because it would run under the new hitstun or guard state. Starting one response discards its sibling requests from that recovery episode, including lower-priority ones with later due steps; otherwise two could run from one hit. A throw is its own episode and inherits nothing from the preceding strike. Chance is rolled once when the episode opens (`preparePrebuffer`), so a primed reversal and its request agree, and a declined roll leaves the native input history untouched. `kTrainingTriggerRandomMacroSlot` resolves to a real slot when the response starts, so recording or clearing a slot in between is honored. A prefix that produces the move while the dummy is still locked calls `markEventAnswered`, or the latch would fire the move again when the reversal ends. If a prefix turns out to be aimed at a chain link, it is cancelled, and the ordinary path answers late, because late is worth having and doubled is not.

**Throw facts.** Throw reception (actions 200 and up) does not lead to a knockdown: it holds the victim, then returns straight to a free action, so it counts as a wakeup path. Exact throw admission needs the dispatcher and the paired actor as well, so stray values in that band cannot open an episode. Fighter processing order exposes one real intermediate tuple during a successful tech, with the dispatcher at 72 before the thrower's own action moves from 70, which is what `combatThrowAttackerTupleIsHold` covers.

**Frame advantage.** `PrimaryRecoveryDisposition::Continued` means the authored script carried the action forward, which the character data proves. `PrimaryRecoveryDisposition::Rechained` means the player cancelled into another attack. Both advance the epoch and declare nothing, but only the first can be read as conventional per-move advantage; a rechain re-anchors the measurement, so a blockstring reports the last move's number.

A gap is the actionable time between one lock ending and the next contact, and it belongs to the contact that closes it, so it is discovered and shown at that instant (`kFrameAdvantageGapDisplaySteps`). `applyGapToEpisode` shows nothing if a number was already resolved and on screen, since a second number beside it would read as a change. A candidate is consumed by exactly one contact whether or not it qualified. It records the pair it was seen on, so a counter-hit reads as a punish and not a gap. Anything over `kFrameAdvantageMaxGapSteps` is treated as being left alone.

**Diagnosing a response.** The session folder holds `trigger_journal.log`, one line for each decision on every event, and `reversal_trace.csv`. `[training-response]` is written for the outcome: expected and actual action, latency, and guard-yield steps, where a nonzero yield means the dummy walked a motion in while stunned. `[training-wakeup] not primed` names the refusal, and the prime-closest values say how near it got. `[training-seed]` prints what the head holds; if that is ever not the guard direction, the seed has taken the block. A `TriggerSuppressionReason` explains a decision already taken and is never read back into one.

In the native parser gate, bit 0 permits ordinary post-recovery normals and bit 2 opens character commands during guard actions, so guard cancel has its own test (`trainingNativeGuardCancelReady`). Merging them would fire an After Block recording several frames early.

A random recording uses `kTrainingTriggerRandomMacroSlot`, one past the last real slot so existing values keep their meaning. It resolves when the response starts, so slots recorded or cleared after the trigger was set count correctly. `TrainingTriggerEvent::GuardCancel` is not an alias of `AfterBlock`: the game opens its command parser during guard when the authored frame exposes permission bit `0x04`, and stand, air and crouch guards reach that window at different points. A latch that arms and never fires looks the same from outside as one that never arms, so `TriggerJournalKind` and `TriggerJournalCause` name each decision for all events, and they are append-only because scripts read the session file back.

### Reversal scheduling

A prebuffered reversal has to reach the game on the first tick the fighter can act. Only generated, zero-delay moves qualify, because a recording has no proven activation frame and acting before the reaction ends would redefine a configured delay. The event comes from the dummy's current reaction family, not from the contact, so a hit that launches has no After Hitstun reversal and one that knocks down becomes `OnWakeup`.

**Three independent terms.** The schedule keeps separate how many ticks until the dummy can act (the state model, from the authored chain), how far ahead the button must go (the input model), and how many direction steps precede it (`commitIndex`). The prime condition is `remaining == commitIndex + 1 + kTrainingChainLastTickIsActionable + lead`. The middle constant exists because `trainingWakeupLockedStepsRemaining` counts the tick the chain ends on, and the terminal is processed within that tick, so the fighter is already free on it. Prime and release must both apply the same lead (`appliedLead`), or the prefix moves earlier and never releases.

**The commit is held.** The native parser gives a button two chances on different ticks: a hold on the second held frame and a tap on its release edge. A one-tick press yields only the release parse, which reads the permission at the top of the tick, before the knockdown transition happens during it, so normals are refused. Specials can still match from ring history, which is why this failure looks like normals broken and specials half working. `kBufferedCommitHold` repeats the press to produce both parses. The first accepted parse wins and the busy fighter refuses the other, so nothing fires twice.

**The model measures its own error.** The state model has known blind spots: what a handler picks after a terminal is not in the authored data. Each attempt records `predictedStartStep`, and `trainingReversalLeadObserve` folds the difference into a per-character, per-move lead. It applies the difference whole, because ticks are integers and the engine is deterministic, so an average would only smear a real change. Only a prebuffered attempt whose prediction survived feeds it. The lead is clamped to `kTrainingReversalLeadMax`, since a larger error is a different bug and a big correction would ask for a `remaining` no knockdown reaches. An unprimed episode produces no measurement to correct a bad lead, so `trainingReversalLeadDecay` walks the `OnWakeup` move's lead one tick toward zero when a knockdown episode ends without priming. The table is bounded (`kTrainingReversalLeadSlots`) and cleared each generation, because a lead from another matchup could stop the reversal priming for good.

**Chain links.** A knockdown can run several actions, and the walk only follows the current one to its terminal. What follows a terminal is chosen by the native handler and is not authored, so prediction cannot tell a link from the last link. If the sequence has finished and the dummy is still in a reaction, the prefix is torn down and `episodeAttempted` blocks another try, since re-priming would walk a visible duplicate motion on wakeup. The ordinary path then answers late instead of not at all, and the response line reports `prebuffered=0`.

**Stun events.** Block and hit stun use `trainingStunRemainingSteps`, not the knockdown walk, whose preconditions a stun never satisfies; verification must use the same walk as the prime or the prefix tears down a step before its commit. The move's motion goes straight into the ring (`trainingSeedMotionIntoRing`), so the prefix presses only the button and its tail. Guard holds fill the gap (`trainingPadReversalSequenceWithGuard`), sized by solving the prime condition and capped by `trainingReversalGuardPad`.

The prefix does not pause for the guard. Blockstun has active attack records almost throughout, so yielding would stall the prefix until the stun ended, and the guard contributes nothing on a generated macro frame anyway. On the last locked step `releaseGuardForResponse` drops the direction, otherwise the held down-back becomes a crouch, then a walk, before the move. This is safe natively, because guarding is decided at contact from the ring head and nothing can land after the last stun frame.

The early release calls `markEventAnswered` at once, because a fighter mid-reversal is not response-ready and the armed latch would otherwise fire a second motion the moment the reversal ended.

### Trigger diagnostics

These files go in the session directory and are pure observers.

| File | Enabled | Answers |
|---|---|---|
| `trigger_journal.log` | Always | One line per controller decision, with event, kind, cause and step. It separates a latch that armed and never fired from one that never armed |
| `training_state.log` | By default, switchable in the menu | One line per change: action transitions, armed and queued responses, suppression reason, and the guard decision |
| `reversal_trace.csv` | While a reversal is primed, plus `kTrainingReversalTraceTailSteps` | Every step of the window, capped by `kTrainingReversalTraceRowCap` |
| `frametrace.csv` | Menu | One row per fighter per admitted step, to compare engine playback with the authored frame list. An unreadable fighter is skipped, not written as zeroes. A cursor of 65534 is the authored terminal sentinel |

`trainingDrainTriggerJournal` runs every step, because the controller's ring drops when full, and an undrained ring would report every later decision as a drop.

Reading a late reversal:

- `forced` in `reversal_trace.csv` and the `[training-wakeup]` release line is the fighter's reaction queue, which the game consumes at the top of the next tick before parsing input. A queued reaction at the press means the attack had already connected, so no command could have beaten it. Empty with hitstun arriving anyway means the fault is ours.
- `dir` and `dwell`, and `btn`, are the game's own ring and hold counters. Read them rather than guessing from a mask. `physicalP2` is the dummy's physical mask.
- `[training-parse]` logs the three ticks after a release. The next-action and transition fields separate "accepted, handler took another tick", "parser matched the wrong command" and "refused, or the parse never ran".
- `[training-wakeup] not primed` gives the walk's reason and the `remaining` it had, and a late response with `prebuffered=0` is the fallback path.
- In the state log, `emit` is what the dummy actually holds, which can differ from a correct guard decision. `guard-yielded` on the response line counts the steps a response held the lane while a guard mode was selected.
- `QOH99_TRAINING_STATE_PROBE=1` logs raw action fields on change, for labelling fighter states.

The response watch steps over free-movement actions while a specific action is expected, because a motion's opening direction produces a crouch or walk first. A response that truly does nothing returns to those actions and times out as no action.

### Hitbox overlay

`HitboxDisplayMode` selects native flag display or the custom overlay, and `hitboxVanillaFlagValue` guarantees they are never both on. The custom path:

1. `trainingPublishHitboxSnapshot` walks fighters, active partners and the clone pool. `trainingReadActorBoxes` iterates `kTrainingHitboxCategories`. The secondary list (`HitboxBoxSource::Secondary`) is what the game's clash resolver uses and the native display does not draw, so a box can clash outside the visible attack box in native view.
2. `hitboxRecordsToDrawFor` handles the count sentinels. A value of 254 means none and 255 means one record, except in the attack list, where both mean none. Neither is ever treated as a literal count.
3. `TrainingHitboxPublisher` passes a `HitboxSnapshotV1` of up to `kHitboxSnapshotMaxBoxes`, using the same buffering as status.
4. `hitboxComposeOverlayFrame` in `src/training/TrainingHitboxOverlayBridge.h` turns boxes into overlay commands, and a `static_assert` in `TrainingHitboxOverlay.h` keeps `kHitboxSnapshotMaxBoxes * kHitboxCommandsPerBox` within 192, the value of `kOverlayMaxCommands`. That bound covers boxes only, so the composer also stops at `kOverlayMaxCommands`.

Timing is the subtle part. `trainingInstallRenderPhaseHook` hooks the game's draw phase through `hkTrainingPreBoxRender`, so geometry matches the frame being presented. If the signature check fails, publishing falls back to the outer tick and trails the picture by one frame. The same code serves replays through `g_replayHitboxOwner`.

**What the native display gets wrong.** These are facts about the game's own F7 view, and they are why the custom one exists. It never draws the secondary list. The clash predicate resolves the defender through that list every time, and the attacker too whenever the attack list is masked or carries a sentinel count, so a clash looks like two boxes not touching. It ignores the active latch, so it draws attack boxes that cannot connect. It gives clone-pool actors attack boxes only and partner slots none. It mishandles count sentinels, drawing phantom boxes for body counts and running past the action's range for hurt counts. Its display list is capped, drops silently and appends the clone pool last, so projectile boxes vanish first. Its red hurt pass is a duplicate that is painted over and invisible, using half of that budget.

**The active latch is not the frame byte.** The frame byte is a set signal: the actor tick copies it into the contact flags (`kTrainingFighterContactFlagsOffset`), where it stays set while later frames use attack records without repeating it, until collision clears it. Sampling the frame byte would show every active frame after the first as inert. `kHitboxContactAttackActive` tests what the engine's predicate reads. `kHitboxContactClash` is set on both actors when a clash resolves, and is read even when neither box was drawable.

**Sentinels.** An attack count of 254 or 255 is not a short list. The engine abandons the attack array on such a count and resolves the attacker through the secondary list, so drawing the record at that index would show a hitbox the engine has decided does not exist.

**Geometry.** The authored edge is inclusive, so right and bottom carry +1, and dropping it underdraws by one pixel exactly when someone asks whether two boxes touch. Facing mirrors about actor X + 128, a property of the authoring space and not of any sprite's width. Coordinates are in the game's 320x240 surface (`kHitboxVirtualWidth`), because the wrapper presents that source directly. A 640x480 canvas would draw everything at half scale, looking right only at the origin.

**Why the phase hook needs one producer.** The native viewer appends boxes to the same display list as its sprites, so it has no latency to correct. Ours reads and draws at different moments. By the time the tick returns, the effect pool has advanced and the frame has committed, so the error is a difference in state, not a constant lag, and neither a one-frame hold nor publishing after the tick fixes it. With the detour live, `trainingPublishHitboxSnapshot` refuses non-render-phase callers, or an off-phase snapshot could overwrite the good one. The fallback path is gated on the frame counter, because the outer dispatcher runs many times per frame during the offline limiter's spin. The detour must not use that gate: it runs before the commit that bumps the counter. Mode-off handling sits above the gate so turning the overlay off works from either path.

**Why the presenter never reads actors.** The fields naming an actor's current frame are updated separately, so a present landing between them could pair a frame descriptor with another action's frame index and dereference the result. The game thread publishes finished screen-space rectangles, and the render thread receives numbers it cannot dereference, so the overlay cannot affect the game. A failed seqlock read keeps the previous view, because composing the pending buffer would draw a torn list.

**Drawing.** All fills go first, then all outlines, because boxes overlap constantly and an outline under the next fill misstates where a box ends. The axis line is emitted first and the clash marker last. The marker derives from the clash rectangles in the snapshot, not the engine's contact midpoint, because the game's own consumers of that value disagree about its Y convention. Whole boxes drop from the end of a fixed order and are counted in `droppedBoxes`, so the overlay can say it shows a subset. A wrapper that ignores the owner and kind flag bits draws everything, so a mismatch never hides boxes.

### The generated catalog

`src/training/generated/TrainingActionCatalogData.inc` is generated. Do not edit it by hand. `generate_training_action_catalog.py` takes the external character-data tool through `--attack-cli`, and game roots through `--november-root` and `--december-root`, exports all 35 fighter resources for both builds, and refuses to write unless they classify identically. Character timing values are left out on purpose. The surviving frame numbers belong to the native behavior rules.

`src/training/TrainingActionCatalog.h` gives three views:

| API | Answers |
|---|---|
| `trainingActionMeta`, `trainingDispatchFamily` | What an action id means: per character for `trainingActionMeta`, per dispatch value for `trainingDispatchFamily` |
| `trainingNativeMoveTransition`, `trainingActionAllowsConventionalFrameAdvantage` | Whether a transition is a native automatic continuation, which frame advantage must not report as an ordinary recovery |
| `trainingMove`, `copyTrainingMoveSequence`, `trainingMoveTriggerEligible` | The move list and input sequences triggers can perform |

`copyTrainingMoveSequence` is shaped by the input parser. The game reads a tap on its release frame and a hold on the second held frame, using the direction at that moment, so the sequence holds the direction one step past each button.

The count constants in the header, such as `kTrainingMoveCatalogRecordCount`, are `static_assert`ed against the generated file, so a regeneration that changes the data fails to compile until the header agrees. `kTrainingMoveMaximumPerCharacter` must stay within `kTrainingTriggerMaximumMoveIndex`, because trigger move availability is one `uint64_t` mask.

Move directions are facing-relative, and a sequence starts and ends neutral. It belongs to the dummy's exclusive macro playback and must never be ORed with human or recovery input. If the bounded array cannot take the extra release step `copyTrainingMoveSequence` wants, the original notation is kept, since a clipped command is worse than a late one. Live partner and form ids are normalized to the selectable owner separately from the character-resource mapping, because a move trigger sends the player's command and does not write into whichever actor is visible. `trainingMoveCatalogFingerprint` lets the wrapper refuse a mismatched catalog, so a one-based move index cannot select a different command.

The catalog stores directions relative to the actor (6 forward, 4 back), and the runtime applies the live facing when it emits each step. Action 73 is the accepted throw-tech result. The thrower is assigned 72 as a counterpart recoil, so 72 is neither an attack nor the result.

### Native seams

`installTrainingHooksOnce` runs only for a training launch. `trainingVerifyNativeSeams` must pass. Otherwise `installTrainingHooksOnce` reports `TrainingDisableReason::UnsupportedStepBoundary` and returns false, and startup rolls back the whole hook transaction. Optional hooks qualify separately through `trainingResolveQualifiedHook`, which checks the symbol catalog and the code bytes, and they feed `trainingCapabilityFlags`. The result reaches the overlay as `TrainingCapability` bits in the status block.

`src/tests/production/test_production_training.cpp` reads `TrainingGameIntegration.inl` as text and anchors checks on function signatures. Forward-declaring an existing function, or moving one of those anchors, can make a check pass on the wrong slice, so read the matching test before reorganizing that file.

**Recovery qualification.** `qualifyRecoveryEvaluator` is why non-Off recovery modes may run at all. It resolves the manual-recovery evaluator, command matcher and ring recorder through the active profile, requires the catalog to agree on the same row by function and by symbol, and compares the loaded bytes with catalogued prologues. It only resolves, reads and compares. It makes two passes so that "resolved but wrong bytes" and "did not resolve" stay distinct, and each `QualificationFailure` points somewhere different: no profile is a launcher problem, unresolved is a model problem, and a mismatch is a wrong image. Static data has no prologue, so `DataRequirement` checks only the address. A pass says the transcribed code is what is loaded, and says nothing about whether recovery works in play. The result is a fact about the process, so it lives beside `RecoverySettings` and `trainingResetRecoverySettings` re-applies it after each reset. Zero comparisons never qualify.

**Native F-keys.** The native scanner latches its developer key bytes on every frame whatever the extra-function config says. That config gates only the joystick block and most of the dispatcher, not the F4 toggle or the F12 freeze, so `clearOwnedNativeDeveloperEdges` is load-bearing. Each key would mutate state the invariants assert on, and leaving one live would fail training closed. The physical keys still work, because the managed keyboard worker is a separate device object delivering `heldTrainingActions`. Reading the latch as well would give one press two producers, which double-toggles F7. The extra-function config stays zero, because arming it adds a joystick poll inside the hooked gate on every tick.

**Include placement.** `TrainingGameIntegration.inl` expands inside an unnamed namespace, so its headers are included at file scope, before that namespace opens. Inside it, each class would become a distinct type from the one its `.cpp` compiles and members would go unresolved at link time. An inline bridge with a function-local static would gain a private copy of its flag.

**Other native facts.**

- `kTrainingFighterActionStateOffset` selects the resource action and `kTrainingFighterDispatchActionOffset` the executable handler, and they can legitimately differ. No single field is an actionability predicate.
- Ring writes are refused while `kTrainingCommandDirectionKillOffset` is set, because that latch forces recorded directions to zero across round transitions. A tap made there records as neutral, so it must read as not eligible.
- A nonzero `kFighterRoundLostOffset` blocks every transition commit, including techs.
- An actor pool slot is live when its character byte is not 0xFF, which is how the allocator scans (`trainingActorSlotLive`). The declared extent exceeds what the allocator searches, so the tail may be uninitialized, hence the extra sanity terms.
- The per-frame box phase has a single call site just before the native draw loops, past the limiter's early returns, which is what makes `hkTrainingPreBoxRender` usable.

**Order against the hook transaction.** `trainingQualifyRecoveryEvaluator` runs from `installTrainingHooksOnce`, after the host image has selected the runtime profile and before the single `MH_ApplyQueued`. The bytes it compares are therefore pristine; run later, it could compare a trampoline against a prologue. Its reader `trainingReadImageBytes` goes through `rangeReadable` and not `se::verifyCodeBytes`, which dereferences unguarded: a probe that crashes while proving the image is wrong has not failed closed. The reason stays `NotAttempted` on a rolled-back transaction, because "no check ran" must not read as "a check failed".

All training hooks use `MH_QueueEnableHook`, never a direct enable, which would suspend and resume every thread. The exact-contact and action-completion seams form one atomic group, so a mismatch removes the whole group instead of publishing partial controls. The throw-pair installer shares an entry address across the two builds, but its body is semantically edited, so it stays native-symbol-only with its bytes checked per profile. Optional contact-policy hooks install after the mandatory group, each on its own, and a failure disables one control with a reason. The render-phase hook is optional too and falls back rather than taking the mode down.

**Contact detours override a decision only.** They let the native code produce every consequence and write no chip, gauge, meter or hitstop. `trainingContactPolicyActive` tests `onClockThread`, because that thread is pinned from the real frame dispatcher on the first frame. The RNG bridge never skips, repeats or redirects the native draw, so the seed advances identically.

**Reusing the native F-keys.** `trainingClearNativeDeveloperEdges` keeps the game's own F-key edges dead, because each mutates state the invariants assert on: F6 is the full round reset, F4 flips the flag described under Resources, F2 and F3 hand a fighter to the COM, and the F12 block writes the freeze byte. Letting one through fails training closed. The physical keys can still be bound, because the native latch is filled by the game's own keyboard scanner while a training hotkey arrives as a `heldTrainingActions` bit from the managed worker. Each action must have one producer, or a single press fires twice, which on a toggle looks like the feature is broken.

`TrainingAction` values never drive the native pause byte: `TogglePause` and `FrameStep` go to `TrainingHold`, since the qualified gate requires that byte at zero and a second owner would fight the config overlay every frame. Only actions in `trainingImageMutatingActionMask` get a pre-step snapshot. `ResetPositionRoundStart` is in it, because without one a failed reset has nothing to roll back to and `trainingRestoreScratchOrFailClosed` would end the session. `ReturnToCharacterSelect` is not in it, matching `commandOwnsNativeImageTransaction`: an action and the command it drives must agree on what they may undo. `RefillAll` has two behaviours on purpose, as `kTrainingLevelActionMask` shows: a tap toggles the preset and holding keeps the meter full.

## Platform layers

These subsystems sit around the rollback core. They share one design habit: anything that can vary between machines (a device, a sound file, a language, a crash) stays out of the simulation, and anything that must match between machines is compared exactly before a session starts.

### Input

Input crosses two processes. The caster turns editable profiles into an immutable binary config. The game process samples devices on worker threads and evaluates that config once per capture.

```
qoh99caster-controls.json
  -> parseControlsFile -> ControlsFile / InputProfile              (caster)
  -> compileInputProfile -> InputLaunchConfig + CompiledModeControlsV1
  -> IPC config, guarded by InputConfigEnvelope                    (crosses processes)
  -> KeyboardWorker / ControllerWorker -> PublishedSnapshot         (game, worker threads)
  -> InputEvaluator::evaluate -> 16-bit canonical mask              (game, capture)
  -> g_netInject -> hkInputRead -> the game's own input consumer
```

**Profiles (`src/input/InputProfile.h`).** A `ControlsFile` holds up to `kMaxProfiles` profiles, the active profile per player, and the mode-control bindings. A `ProfileBinding` names a physical predicate: a DIK scan code, or a `ProbeKind` plus token, polarity, hat bit and press/release thresholds. `ProfileDevice` records how to find the pad again.

**Compiler (`src/input/InputCompiler.cpp`).** `compileInputProfile` is the last point where a bad profile can be explained to the player, so it refuses rather than repairs:

- Every one of the 8 fight actions needs at least one binding, and at most `kMaxBindingsPerAction`.
- One physical predicate cannot drive two fighter actions. `predicateIdentity` ignores thresholds on purpose: the same axis direction is the same predicate at any threshold.
- A mode control in a domain that overlaps the fighter cannot share a fighter predicate, and overlapping mode controls cannot share one (`modeDomainOverlapsFighter`, `modeDomainsOverlap`).
- Controller probes are interned into `InputLaunchConfig::probes`, at most `kMaxControllerProbes`. Bindings then carry an index, so a worker reads each probe once however many bindings use it.
- A standardized probe with no mapping identity is an error. So is a controller binding with no selected device.

A spectator compile returns an empty config, and `validateInputLaunchConfig` rejects a spectator config that carries probes or bindings. Spectators therefore have nothing to sample.

`InputLaunchConfig` is plain data. `DllMain.cpp` checks the envelope's magic, schema, byte size and `hashInputConfigBytes`, then runs `validateInputLaunchConfig` and `validateCompiledModeControls` again. For netplay, spectator, replay and training launches a failure refuses the launch. The game process never trusts the caster's validation alone.

**Workers (`src/input/InputWorkers.cpp`).** Each worker runs below normal priority, polls about every 4 ms, and publishes through `PublishedSnapshot<Payload>` in `src/input/InputSnapshots.h`. This is a two-slot exchange with a hazard index. `publish` skips if the reader holds the target slot, and `read` retries at most `kSnapshotReaderRetryCap` times. Neither side blocks, so a stuck HID driver can stall a worker but never the game thread. A worker that will not join within its deadline is leaked, not unloaded.

**Evaluator (`src/input/BindingEvaluator.cpp`).** The hook code in `src/GameHooks.cpp` treats a snapshot as healthy only if its status is `SnapshotStatus::Connected` and it is younger than `kInputStaleThresholdMs` (100 ms). `InputEvaluator::evaluate` then applies these rules:

| Rule | Mechanism | What it prevents |
|---|---|---|
| Neutral gate | Per-source `SourceGateState`: `Disconnected` -> `AwaitNeutral` -> `NeutralSettle` -> `Armed` | A button held during a reconnect, or during an alt-tab back, firing as a fresh press |
| Serial barrier | After unhealthy or focus loss, the gate ignores samples until `sampleSerial` moves | Judging neutrality from the sample taken before the outage |
| Device generation | A new `deviceGeneration` resets the gate and that source's axis latches | State carrying over to a re-opened device |
| Stall hold | An armed source going unhealthy keeps `lastArmedMask` for up to `kSourceStallHoldEvaluations` | A short worker hiccup dropping a held direction or charge |
| Axis hysteresis | Latch on at `pressQ15`, off at `releaseQ15`, per binding | Chatter around one threshold |
| SOCD | `SocdPolicy` on each axis, applied after the keyboard and controller masks merge | Opposite directions reaching the game unless the policy is `Native` |

Focus loss under `FocusPolicy::GameWindowOnly` disconnects immediately and has no stall hold. The output is masked with `kCanonicalFightMask`.

**Rollback never touches a device.** While netplay is active, `hkInputReadResolved` returns `g_netInject[player]` for players 0 and 1, or 0 until the session is running, without reading a device. `netManagedPhysicalCapture` samples separately from the game's read, so resimulation replays stored masks. `hkInputReadResolved` is also the single priority list for every other input source: replay injection, training overrides, offline overrides, and last the game's original read.

**Offline VS.** `buildNativeBindingPlan` (`src/input/NativeBindingTable.h`) picks each action's first keyboard binding and the matching keyboard callback, and `src/GameHooks.cpp` writes the plan into the game's own binding table. Native keyboard code then keeps working as designed. Actions it cannot express are reported in `unexpressedMask`.

**Mode controls.** `ModeControlAction` values are append-only and must stay below `ModeControlAction::Count`, because `modeControlBit` shifts by the value into a 64-bit word. `ModeControlEvaluator` only reports a control that has been seen released since the last disarm. A generation change, a focus change or a disabled action disarms whatever is held, so a key held across a mode change cannot fire. `validateBinding` rejects stick predicates on this plane and allows only analog triggers. `LeaveMatch` is bindable, and `installDefaultModeControls` gives it no default binding.

#### Controller backends

`IControllerBackend` has two implementations, and the choice decides what a stored number means.

| Probe kinds in the profile | `probeBackend` | Selected in the game process |
|---|---|---|
| `GamepadButton` / `GamepadAxis` | `Sdl` or `Unknown`; `validateInputLaunchConfig` refuses `DirectInput8` | SDL3 only; if SDL did not load, the pad is off and the keyboard keeps working |
| Raw kinds | `DirectInput8` | `createDirectInput8ControllerBackend`, whether or not SDL is present |
| Raw kinds | `Sdl` | SDL with `preferRawJoystick(true)`; off if SDL did not load |
| Raw kinds | `Unknown` | SDL raw if available, otherwise DirectInput8, with a log line |

`inputCreateBackendFor` enforces this table. The two backends number the same pad's buttons differently, so substituting one for the other would press the wrong buttons rather than fail. A disabled pad is the correct outcome, and the log line says how to fix it.

**SDL3 (`src/input/SdlLifecycle.cpp`, `src/input/SdlControllerBackend.cpp`).** SDL3 is delay-loaded, so `sdlRuntimeAvailable` probes with `LoadLibraryW` before any SDL symbol is touched. A missing DLL therefore logs a line rather than terminating the process. Further invariants:

- `sdlBootstrapOnMainThread` initializes `SDL_INIT_GAMEPAD` under one process-wide lock, with SDL's joystick thread hint on and its events off. Workers poll explicitly.
- If something else already owns the subsystem, the result is `SdlBootstrapResult::ForeignOwner`.
- Teardown releases only this module's reference and never calls `SDL_Quit`.
- The caster calls `sdlReleaseForLaunch` before spawning the game, so the child does not inherit a half-owned subsystem.
- A profile's mapping is applied per instance with `SDL_SetGamepadMapping`, because `SDL_AddGamepadMapping` would change every device of that model.
- SDL regenerates mapping text when read back, so `recordOpenMapping` keeps what SDL returned. `mappingUnchanged` later compares against that text, never against the text that was sent.

**Layout faults.** If `mappingUnchanged` fails after `ControllerWorker::notifyDeviceChange`, the worker closes the device, sets `ControllerAttachState::LayoutChanged` and stops discovery until relaunch. Only the worker tests call `notifyDeviceChange`; nothing in the shipped game process does, so this check does not run in normal play. The compiled bindings do not describe the changed layout, so a neutral pad is safer than wrong presses.

**DirectInput8 (`src/input/DirectInput8ControllerBackend.cpp`).** This backend answers raw probes only and returns false on standardized kinds. It uses `c_dfDIJoystick2`, opens background and non-exclusive, and normalizes axes to the same range SDL reports. `povToHatMask` converts POV degrees to the hat bits the evaluator expects, including both bits of a diagonal. `resolveFocusWindow` falls through supplied window, console, foreground and a hidden popup, because the cooperative level needs a top-level window.

**Finding the device again.** `resolveDeviceIdentity` tries tiers in order: serial hash, path fingerprint plus physical class, exact class, vendor and product. The first tier with exactly one match wins. More than one match is `IdentityOutcome::Ambiguous`, which is never guessed through. The `VendorProduct` tier exists because a GUID captured through one backend does not match the other backend's GUID for the same pad. `DeviceCandidate::liveHandle` is transient and is not identity.

**Why a stall is not a disconnect.** The 100 ms staleness check trips on ordinary USB and scheduler hiccups. The neutral gate exists so a button held across a real reconnect cannot fire as a new press, but it needs two captures with nothing held, and nothing-held is rare for a player guarding or charging. Treating a stall as an unplug would leave the pad dead until the player let go of everything. `kSourceStallHoldEvaluations` is 30 evaluations, about half a second at the canonical frame rate, because the evaluator is stepped per frame: long enough for a hiccup, short enough that a dead pad does not hold "walk forward" for long. No 1-0-1 appears on a held input, so recovery creates no phantom edge.

**Triggers and sticks on mode controls.** A trigger rests at one extreme and leaves it only when pulled, so it behaves as a button with travel. A stick rests at centre and drifts, and a worn stick bound to "load state" would fire unattended. `gamepadAxisTokenIsTrigger` is the single definition, and `validModeControlBinding` applies it. A raw axis is refused because its number says nothing about its resting shape, and some devices combine both triggers on one axis. `ModeControlEvaluator` still uses press and release thresholds, so a worn trigger resting near the line cannot chatter a hotkey.

**The profile file is strict in both directions.** The parser rejects unknown fields and the whole file with them, and a rejected file means keyboard defaults at the next launch. So every key the writer can emit must also be listed in the matching `onlyKnownKeys` call in `src/input/InputProfile.cpp`, and a round-trip test must set any optional field, since an empty one is never written. `parseBinding`'s `digitalOnly` flag describes the retired `sessionControls` JSON shape, not mode controls in general. `serialHash` is 64 bits and stored as a hex string, since the file's integers cannot carry the full range. It comes from `identityStringHash`, which keeps the plaintext serial out of the settings file while still surviving a USB port change, as a path fingerprint cannot.

**One controller, two processes.** The caster's Controls page owns device selection and polls directly on its UI thread through `ControllerPreview`. It releases SDL with `sdlReleaseForLaunch` before launch, so the two processes never compete for a device. The two processes also choose backends independently, which is why `compiledControllerNeedsSdl` decides from the compiled profile rather than from what happens to load. The game's enumeration is the one that decides whether the pad works, so `src/input/InputWorkers.cpp` logs it the first time and whenever it changes. `ControllerAttachState` is diagnostic only: nothing in the capture path reads it. `IdentityOutcome::Ambiguous` is re-evaluated on every discovery pass, because a duplicate seen only during SDL's start-up enumeration must not disable a pad for the session.

#### Diagnosing a controller

| Symptom or log | Meaning |
|---|---|
| `[controller.sdl] SDL3.dll did NOT load` with error 126 | The file is not beside the executable |
| The same line with error 5 | Something, usually antivirus, is denying the file |
| `[controller.di8]` "acquire failed" with the Steam text | Another process holds the pad exclusively, usually Steam Input. Not an open failure: a background, non-exclusive device can acquire later, and `poll` retries |
| `isSteamVirtualController` or `isValveInputDevice` true | Steam took the real pad and republished a virtual one, so a saved identity no longer matches |
| `[controller.migrate] verdict=` | `planDeviceMigration` looked for one replacement that can honour the profile. It only decides; it never applies the result |
| Every row unusable at once | A backend fault, reported once for the page. Backend failure is not a per-device fault |

`planDeviceMigration` treats serials as decisive when both sides have one: matching serials are the same board through any port, and differing serials are a different board even if everything weaker agrees.

**Awkward hardware.** These are facts about the devices, and recognizing one never changes which device is selected or how it is driven:

- **Adapter boxes (`isMultiConsoleAdapterBox`).** The box always reports the same control counts, but the report shape belongs to the console pad plugged in. A Saturn pad's d-pad arrives on full-scale axes with the hat silent, and a PlayStation pad's arrives on the hat. A profile therefore fits one pad type, and changing pad type needs a rebind. Binding by position (`preferRawJoystick`) is the correct path here, not a fallback.
- **Multi-port boxes.** Each port enumerates as a joystick with the same vendor and product and usually no serial, so the identity tiers answer `Ambiguous`. Using one port avoids that.
- **Identical boards.** With no serial or distinguishing path, the only evidence left is the player. `beginIdentify` opens the tied candidates together and `stepIdentify` waits for one decisive edge, at a threshold stricter than ordinary capture so stick drift cannot answer.
- **Boards with no directional control.** Leverless and custom firmware often reports each direction as a separate button, which cannot form a movement cluster. `directionsMustBindIndividually` sends the player through single-control binding instead.
- **DirectInput8 triggers.** Xbox-class pads report both triggers on one combined axis, so they cancel. Separate triggers need SDL.
- **The DirectInput8 focus window.** It must be a real top-level window, though it may be hidden. A message-only window is rejected.

**Native input facts.** The game's facing swap tests Right first and clears both horizontal bits, so Left+Right becomes Left, while Up+Down stays simultaneous (`applyNativeFacing`). The device layer never applies facing. The native binding table holds exactly one descriptor per action per player, which is why offline VS can express only the first keyboard binding. A `sourceType` of 0 is keyboard and 1 to 4 are WinMM joystick ids, and each descriptor needs its matching callback or the table is inconsistent. `KeyboardScanCodec` rejects sequences that cannot round-trip, such as PrintScreen's fake-shift prefix, rather than guessing a key.

**Pad control of the overlay menu.** The wrapper cannot see pads: its hotkey table holds DIK codes only, and ImGui's gamepad navigation stays off because its backend reads XInput directly and would bypass the player's bindings. `MenuNavigationForwarder` therefore drives the menu from the same logical masks the game receives, OR'd across the offline players, so either pad can change the dummy's settings. It fires once per press with no auto-repeat, matching the keyboard path, and is `reset` whenever the menu is closed so a button held at close does not eat the next press. `OpenOverlayMenuPad` is separate from `ToggleOverlayMenu` on purpose. Evaluated edges carry no device attribution, so one controller-capable action would fire in both modules and open and close the menu in one press. It defaults to Start, the button a match does not use. Its low value matters, since an action past bit 63 would shift off the mask.

### Audio

The game's sound calls fire from inside simulation ticks, and rollback re-runs those ticks. Calling DirectSound directly would replay every hit sound on every correction. Instead, `src/audio/` splits audio into intent (what the simulation asked for) and physical playback (what the speakers did), and reconciles the two after a correction.

**Disposition.** Before each owned tick, `src/GameHooks.cpp` sets an `AudioTickDisposition`:

| Disposition | Used for | Journals intent | Native audio |
|---|---|---|---|
| `ForwardVisible`, `ForwardHidden` | New simulation | yes | deferred to flush |
| `HistoricalCorrection` | Rollback resimulation | yes, staged | forbidden |
| `SpectatorConfirmedVisible` | Spectator at the head | yes | deferred to flush |
| `SpectatorConfirmedCatchup` | Spectator catching up | yes | forbidden |
| `ImmediateOffline` | Not rollback-capable | no | direct |
| `PhysicalFlush` | The runtime's own emission | no | allowed |

The choice to journal is tied to `rollbackEligible` and `netLifecycleTransitionRequiresExactInput`. Ticks that rollback cannot revisit are not journaled, and the two predicates must change together.

**Capture.** Hooks such as `hkSoundDuplicatePlay`, `hkSoundLoad` and `hkSoundStop` check `capturesIntent`, append an `AudioCommand`, and return what the native call would have returned. `hkBgmCommand` checks the same flag but queues into `QohBgmBoundaryGuard` instead. `AudioCommand::setPath` copies the path, because the game builds paths in stack buffers.

**Audio is not a determinism input.** The simulation reads sound status through a virtual model (`virtualSoundStatus`, keyed by `kSimulationStatusSoundIds` in `src/game/DeterministicAudioPolicy.h`). `hkSoundLoad` reports those sounds as loaded whether or not the file exists. Two installs that differ only in `.wav` files stay in sync, which is also why the content roots below exclude them.

**Journal (`src/audio/AudioTimeline.h`).** `AudioTimeline` keeps a ring of `kTimelineFrames` frames, and `journalFor` checks the stored frame number so a recycled slot cannot answer for another frame. `commitCapture` normalizes a stop-then-play on one endpoint into a single `AudioKind::Replace`. During a correction, journals go to a staging array, so an aborted correction leaves the committed history untouched.

**Forward flush.** `flushPending` runs after a visible frame. The newest pending frame emits in full. For older hidden frames, non-looping plays beyond the age horizon are skipped (`AudioHorizonConfig`: 2 frames for players, 6 for spectators), and plays superseded by a later play on the same endpoint are coalesced. Stops and loads always run. Each emitted play records an `EmittedOccurrence` holding a `PhysicalToken`.

**Tokens.** `QohDirectSoundSink` issues a token per play and tracks the current owner of each endpoint. `stopIfOwner` acts only if the caller's token is still the owner and the device generation matches, so a stale correction cannot cut off a newer sound. `hkAudioTeardown` calls `onDeviceTeardown` to bump the generation. A muted sink (`flushPendingMuted`) still issues tokens, which keeps the books correct while a replay seeks.

**Correction.** `beginCorrection` removes queued BGM in the range and starts staging. `commitCorrection` calls `reconcileCorrection` in `src/audio/AudioReconciler.cpp`. The reconciler is pure: it folds old and corrected journals into per-endpoint histories of `Generation` records, then matches them newest-first per endpoint. A match needs an equal `AudioSemanticKey` and start frames within `kMatchMaxFrameDistance`. The key deliberately omits the frame number, so a sound that moved by a few frames counts as the same sound.

| Case | Action |
|---|---|
| Matched, played, active, token still owns the endpoint | Nothing audible; the sound keeps playing and the record moves to the corrected frame |
| Matched, played, active, token stale | `PlayAtCursor`, unless the sound has expired |
| Matched, played, already ended | Keep the record only |
| New, ended or expired by the frontier | Skip |
| New, active, age at most 2 frames | Play normally |
| New, active, older | `PlayAtCursor`, so it resumes at the position it would have reached |
| Old only, played | `StopIfOwner`, unless it ended and its duration has passed |

Actions are ordered stops and resource changes first, then plays, with `tokenFromAction` remapped to match. For `PlayAtCursor`, `SinkAction::cursorBytes` holds an age in frames when it leaves the reconciler. `executePlan` converts it with `resumeCursorBytes`, because the catalog belongs to the executor.

**BGM.** `QohBgmBoundaryGuard` queues music commands only from the two callsites in `bgmOwnedCallsite`, and suppresses a repeat of the latest track and flags. `drainConfirmed` plays a command only once its frame is at or below `confirmedThrough`, which `src/GameHooks.cpp` limits to `remoteContiguousThrough`. Music therefore never changes on a prediction.

**Failure.** The tables are fixed size. Overflow sets `failedClosed`, which disables physical audio for the session and logs through `QLOG_ISSUE`, while the match continues. The stricter cases go through `netFailClosed`: a failed `commitTick` or `commitCorrection`, and a historical wave-parse cache miss, since resimulation may not read the disk (`audioWaveParse` with `allowFilesystem` false). A correction window whose committed journals contain `LoadAttempt` or `Unload` also fails the commit, because resources cannot be safely replayed. `formatLedger` prints most counters and is the first thing to read when audio misbehaves.

**Why the journal lives outside snapshots.** Restoring game memory must not erase the knowledge that a sound already reached the speakers, so `AudioTimeline` and its emission records stay out of rollback state. `commitCorrection` on the timeline does not clear emissions for replaced frames: the reconciler consumed them to build credits, and the caller records corrected emissions as the sink executes the plan. A replay takeover is a separate local transaction (`beginReplayTakeover`, `restoreAfterReplayTakeover`, `endReplayTakeover`); netplay correction uses only the correction calls. Each pending frame keeps the disposition it was committed under, since the horizon and coalescing are facts about that frame and not about the flush.

**Identity rules that are easy to break.**

- Sample id alone is not content identity. Character-bank changes reuse ids for different WAVs, so each play resolves `assetGeneration` at capture, and a token for one asset cannot credit a corrected play of another in the same sample.
- A bare play and a stop-then-play are the same onset, so the reconciler canonicalizes them before comparing.
- If the asset is absent, the native play returns early and the explicit stop still takes effect. That pair therefore normalizes to a `Replace` with no sample (`LoadOutcome::Absent`) and is never dropped. Two legitimate plays are never collapsed.
- Newest-first matching is what solves the sequence "A, stop, A" corrected to "A": the live corrected occurrence pairs with the latest old one, and a stale earlier generation cannot take it.

**Physical details.** Native playback starts before a caller can seek, so `PlayAtCursor` plays and immediately repositions, which leaves a brief audible window. If `setCursor` is missing, the sound restarts from zero. The cursor is age times byte rate over the canonical 60.60 Hz, rounded down to block alignment (`resumeCursorBytes`), and `kCursorExpired` means skip it. `silenceByteFor` exists because streaming silence differs by depth: 0x80 for 8-bit PCM and zero for 16-bit. `parseWaveFacts` accepts a missing final pad byte after an odd data chunk, because several stock character WAVs omit it and DirectSound accepts them. Local mute and volume stop at the sink, so they never change intent, digests or reconciliation.

**BGM dedupe mirrors the game.** The native dispatcher reissues its desired track every gameplay frame, and the native function ignores a request equal to the current one. The guard copies that equality against the latest canonical request, so a real transition such as A, none, A queues all three. Drain does no equality check, because native code restarts on every command that gets through. Drained history is never revisited, since it already happened.

### Content and compatibility identity

Rollback needs both sides to simulate byte-identically, so the project proves it before a session starts. Two layers do this.

**Local admission.** `validateRuntimeInstallationForAdmission` in `src/client/BuildIdentity.cpp` is the gate. `src/client/Main.cpp` calls it at the game-file check and `src/client/Launcher.cpp` calls it again before launch.

1. `detectKnownRuntimeExecutable` hashes the executable. `canonicalizeKnownExecutableVariant` maps attested variants in `kAttestedVariants` onto their canonical hash, keeping the physical hash for logs. `findProfileByExactExecutableEvidence` then requires hash, size and machine to agree.
2. Exact hashes are the authority, and file dates play no part. The PE timestamp is forensic only. There are two runtime profiles (`kBuildCount` in `qoh99_re/include/qoh99/generated/BuildProfiles.hpp`). The November profile admits the 11/26 retail executable and the 9/4/2006 PovertyHut Jr. community executable. The December profile admits the December build, and `kExpectedProfiles` marks it qualified despite the `DecemberCandidate` enumerator name.
3. `SimulationContentManifestBuilder::build` reads every file in `kExpectedContentEntries` (`.chr`, `.fob`, `system.bin` and two `.img` files) and compares size and SHA-256 per profile. Any difference gives `ManifestFlagRootMismatch`.
4. It also walks the install. An unknown `.chr` or `.fob` sets `ManifestFlagUnreviewedPotentiallyRelevantExtra`, since the game could load it.
5. `ManifestResult::admissionAllowed` requires `InstallationDisposition::KnownQualifiedManifest`, a complete identity, a qualified profile and `bytesVerifiedForAdmission`.

The last condition matters because `hashFile` has a metadata cache for diagnostics. A build with `verifyActualBytes` false may reuse it, and its result can never admit.

**Peer comparison.** `CompatibilityIdentity` in `src/compat/CompatibilityIdentity.h` is a fixed 192-byte record pinned by `static_assert`. `makeCompatibilityIdentity` fills it from the executable hash, the content roots, the state schema, the mod identity and the timing profile. `compatibilityMismatch` compares every field except `peTimestamp` and returns one `IdentityMismatch`. `NetplayCore.cpp` runs it on each hello, and `ResilientSpectatorDistribution.cpp` runs it for spectators. `identityMismatchPlayerText` turns the result into advice a player can act on.

The roots are narrower than the local check. `hasFacet` limits `simulationRootSha256` and `validationRootSha256` to `.chr` files, and `canonicalSerialize` builds them from sorted normalized paths, so path spelling and enumeration order cannot change a root. Local admission asks "is this a known install", and the peer check asks "will these two simulate alike".

**Changing the state schema.** `kStateSchemaCurrent` must change whenever the bytes that are saved or hashed change meaning. Earlier values stay listed in `isSupersededStateSchema`: such a recording is playable but not byte-verifiable, and `src/client/replay/ReplayCompatibilityV2.cpp` applies that rule. The `legacy` argument of `canonicalRoot` recomputes roots under the previous recipe for the same reason. These legacy roots are local only and never go on the wire.

**Why the peer roots stop at `.chr`.** A `.chr` file carries the hitboxes, frame data and per-character rules that rollback replays inputs against, so a difference really does diverge two peers. That demonstrated effect on simulation is the line, and membership of a file list is not. Every other path in a root is one more way to refuse two honest installs. A divergence that slips past the roots still meets the per-frame state hash, which stops the match at the frame it happens. `kSimulationContentSchemaVersion` must change with the recipe, so two peers with identical files are not told their content differs.

**Tripwires and caches.** An unknown `.chr` or `.fob` blocks strict admission without being hashed. It is a request for review and not a classification, because a suffix cannot prove the game loads the file. Only exact membership of the catalog puts a path in a root.

**What identifies a build's files.** The two builds' `yosie/yosie.chr` are the same size and differ in one byte, so a whole-file digest separates them. `content_preconditions` records that as `simulation_probe_path` and `simulation_probe_sha256`, for December only. November states "no probe" with two empty strings, and the generator enforces the asymmetry so adding a build cannot create a new way to refuse a working November install. The December game also refuses to start unless one resource has an exact file timestamp (`editedContentGate`). Any copy or extraction rewrites a timestamp, so the player could not fix that refusal: admission does not copy the check, and the wrapper patches the native gate out when its bytes match (`qoh99_report_december_startup_gate` logs the result). The probe fields currently appear only in the profile's diagnostic text: `kExpectedContentEntries` is what admission compares.

### Translation

`qi18n::tr` in `src/i18n/Tr.cpp` is a binary search over `kJapaneseRows`, a static table keyed by the English string. It allocates nothing and costs nothing in English mode. A miss returns the key pointer itself, so untranslated text falls back to English.

- **Table generation.** `tools/make_translation_table.py` writes `src/i18n/TrTableJa.inc` from a JSON list such as `tools/translations_ja.json`. It is not a build step, so run it and commit the result. It sorts by the UTF-8 bytes of the key because the lookup uses `strcmp`; any other order would make rows silently unfindable. It also splits literals where a hex escape would swallow a following hex digit.
- **Keys are whole strings.** A key must match exactly, including format specifiers, so `tr` runs on the format string before formatting. Do not assemble sentences from fragments.
- **`trText`** handles runtime strings. It tries a whole match, then consumes up to 8 label-shaped prefixes (a key ending in `:`, `(`, `=` or `-`) and leaves the remainder in English.
- **Two copies of the language.** The caster and the wrapper each compile `Tr.cpp` and hold their own `g_language`. The caster writes `[Overlay] Language` to `ddraw_wrapper.ini`, and the wrapper reads it with `setLanguageFromIniValue`.

### Logging and crash handling

**`src/Log.h`.** `QLOG` formats into a 1024-slot ring under a critical section, and a writer thread does the file I/O. The game thread therefore never waits on a disk. When the ring is full the oldest record is dropped and counted.

By default nothing reaches the disk. The writer opens the file only when diagnostics are enabled, through `QLOG_ISSUE`, `Log::issue`, continuous mode, or `QOH99_LOG` set to `1`, `on` or `true`. A clean session leaves no log file. The first issue persists the ring's history as well, so the lead-up to a problem is kept.

The enable flag is a named event derived from the log directory. An issue raised in the DLL therefore also turns on the caster's log for the same session. Session logs live in `logs\session_*` folders, or `session_*.zip` once archived. `Log::markProcessExit` exists because `DllMain` must not wait on the writer under the loader lock.

**Two crash paths.** `src/CrashDump.cpp` installs an unhandled-exception filter in both processes (`qcrash::install`). Faults inside an owned simulation tick are caught closer to the source by `netInvokeOwnedTickWithSeh`:

1. The tick snapshot (`OwnedTickCrashSnapshot`) is taken before the call, while memory is known to be good.
2. The `__except` filter calls `captureExceptionContext`, which copies the exception record and context first, before any module lookup or formatting. It never keeps the `EXCEPTION_POINTERS`.
3. `reportCaughtOwnedTick` runs after the handler, using the copies, and logs frame numbers, rollback depth, characters, actions and the cached compatibility identity. It also writes a dump.

Rules in this code:

- `dbghelp.dll` is resolved once at install time (`probeDbgHelp`), because loading a DLL inside a filter can fault or deadlock. It stays optional, and without it the text record is the report.
- `tryEnterCrashReporter` prevents a crash inside the reporter from recursing.
- `cacheCompatibilityIdentity` stores the identity ahead of time, so the crash path never has to compute anything.

Dumps are named `crash_<tag>_<pid>.dmp` and go in the session directory.

### DirectDraw wrapper

`ddraw_wrapper/` builds the `ddraw.dll` placed beside the game. Its origin is Aquat1c/EfzDdrawWrapper, which is where the `EFZ` export prefixes come from.

**A full replacement.** The wrapper implements the DirectDraw objects itself (`MyDDraw`) and does not forward to the system DLL. Surfaces and their blits live in `surface_ops.c`, and `present_frame` hands completed frames to `EFZDX9_Presenter_Present` in `render_dx9_imgui.cpp`, which also holds the overlay. `QohRendererBackend` selects classic D3D9, D3D9Ex flip or a GDI diagnostic, and `g_presenter` records the requested and effective mode, backend and fallback reason.

**Exports are a contract.** `ddraw.def` fixes names and ordinals. The two modules bind in both directions at run time:

- `qoh99_netplay.dll` resolves wrapper exports by name. A missing export is treated as an optional feature being absent, so a new function left out of `ddraw.def` fails silently.
- The wrapper resolves `qoh99np_presentation_disposition` from the DLL. If that is absent, it behaves as a plain wrapper.

`validatePackagedDdrawWrapper` requires `EFZDDRAW_GetNetplayPacingAbi` and `EFZDDRAW_SetFrameLimit`, and `launch_wine.sh` checks the same two.

**Presentation during rollback.** `Qoh99PresentationDisposition` decides what a resimulated tick may do to surfaces. Under `QOH99_PRESENT_SUPPRESS_HISTORICAL`, drawing and pacing are skipped, but the completed-frame Blt always executes (`qoh99_presentation_executes_surface_work`), because it is the game's only frame-commit boundary. `qoh99_netplay_owns_pacing` moves pacing to the DLL. `EFZDDRAW_PresentHeldFrame` repaints the last image while the simulation is held.

**Startup order.** `wrapper_lazy_init` runs logging, then config, then DPI awareness (which must precede window creation), then import patches, then `qoh99_movie_install`.

**Movie player (`qoh99_movie.cpp`).** The game plays its opening through MCI, and `qoh99_movie_install` patches those `WINMM.dll` imports in the game module. `Hook_mciSendCommandA` serves only the opening movie, identified by `qoh99_movie_mci_open_is_ours` and `QOH99_MOVIE_MCI_DEVICE_ID`, and forwards all other calls. Control flow:

- `qoh99_movie_install` runs inside `DirectDrawCreate`, so it only decides and patches. The DirectShow graph is built later, at `MCI_OPEN`.
- `buildGraph` adds a VMR9 renderer first, because the graph builder could otherwise pick the DirectDraw renderer, which would call back into this wrapper.
- Where the game's opening movie uses Indeo 5, `registerIndeoDriver` registers a decoder for this process only. It tries `ir50_32.dll` beside the wrapper, then `ir50_32original.dll`, then the Windows copy. The DLL is optional. A lossless `Q99V` sibling file is a further fallback.
- Every failure path ends in `postCompletion`, which posts the `MM_MCINOTIFY` the game waits for. An undecodable movie becomes a skipped intro, not a hang.
- With `QOH99_SKIP_INTRO`, `applySkipPatch` rewrites the game's movie probe, but only after `qoh99_movie_skip_patch_site_matches` confirms the expected bytes. Otherwise the intro plays.

#### What the game calls, and why the shape matters

These are facts about the game's own code. It calls two exports. `DirectDrawCreate` is followed by a `QueryInterface` for `IDirectDraw2`, and everything else runs through that pointer, including the 5-argument `SetDisplayMode`. The calls are `__stdcall`, so a 3-argument form would corrupt the caller's stack. That is why `DD_QueryInterface` refuses an unknown interface rather than handing out a mis-shaped vtable. `DirectDrawEnumerateA` is the second export, and its callback skips an entry with a NULL GUID. `DirectDrawEnumerateA` therefore reports the primary display with a NULL GUID on purpose. The game then falls through to its own `DirectDrawCreate(0, ...)`, whereas a non-NULL GUID would make it build and release an extra object. The game's windowed init also aborts if `CreateClipper` fails, so a working clipper stub is required even though presentation ignores clip lists.

The other exports in `ddraw.def` are never called by the game, but any process that loads this file can reach them. Each one calls `wrapper_lazy_init` first and either does the real work or fails with a log line.

#### The frame boundary

The game renders into a 320x240 offscreen surface, then blits it to the primary. In windowed mode the destination is the client area's screen coordinates, while the emulated primary sits at the origin, so honouring that rectangle would clip the whole frame away. `detect_qoh99_frame_scale_blt` recognizes this blit and the wrapper presents its source (`FrameSourceKind`), ignoring the destination position. The fullscreen 320x240 display config reaches the same point through `DDSurf_BltFast`.

`ddw_is_qoh99_frame_commit` identifies the commit by its source: a full read of a non-primary 320x240 surface targeting the primary. The destination rectangle only has to be at least 320x240, because the game derives it from the window's client size. A stricter check would stop frame commits as soon as the window was resized, and the picture would freeze while the simulation kept running.

This blit is the sole game-frame boundary. Rendering can also happen through `WM_PAINT`, resize, `Flip`, fills and `Unlock`, but rollback state advances only here.

**Who may touch the live surfaces.** While `qoh99_netplay_owns_pacing` is true, `present_frame` refuses every non-canonical call, because a surface can hold mid-rollback pixels between ticks. Geometry and expose repaints go through `present_current`, which draws the last published copy with `present_snapshot`. That path consumes no presentation slot and touches no pacing counter. During the opening movie `present_frame` stands down completely, because DirectShow's renderer owns the picture from a child window.

**Two pacing questions.** `qoh99_netplay_owns_pacing` asks who drives presentation timing, and visible replay playback answers yes. `qoh99_netplay_owns_network_pacing` asks whether a remote peer depends on the game thread, and a local replay answers no. A guard whose hazard is stalling a peer, such as the menu toggle in `wrapper_dispatch_hotkey`, must use the second. If the DLL lacks `qoh99np_owns_network_pacing`, the query falls back to the broader one, so the mismatch errs toward a closed menu rather than one that opens during netplay.

**Frame limiting.** `fps_throttle` does nothing while netplay owns pacing, so hidden resimulation runs unthrottled. Its deadline is absolute and fractional, since truncating ticks per frame would drift over a long match. It sleeps for most of the interval and yields near the end. Both an iteration count and a `GetTickCount` limit bound the wait, because an unbounded spin burns a core on Wine and can starve wineserver, audio and the network pump. `EFZDDRAW_SetFrameLimit` publishes only interlocked scalars, and the game thread re-anchors its own 64-bit deadline. A value of 0 hands pacing to the DLL. `EFZDDRAW_PrepareOverlayDevice` exists so the DLL can build the D3D9 device at its startup barrier, while no peer is waiting on the game thread.

**Held frames.** The game draws from inside its gameplay tick. When the tick's gate holds the simulation, the frame body returns before its draw-list flush, so no blit arrives and nothing presents, including the overlay. The native game does not care, because DirectDraw leaves the last image on screen. `EFZDDRAW_PresentHeldFrame` fills that gap and never commits a frame or reports a slot. Its rate ceiling is twice the configured limit, not the limit itself. The caller already runs at about that cadence from inside the game's frame gate, and a declined call does not advance the timestamp, so a ceiling on the operating point would drop roughly half the repaints. The ceiling exists only to stop a runaway if the game's own limiter is disabled, because a spin of full presents would peg the core that delivers the key closing the menu.

#### Window behaviour

**Boot stays at 1x.** Enlarging the client before the game has filled its first surface publishes magnified or partial content, and the wrapper sees no `Flip` from this game to wait for. `SetDisplayMode` therefore only arms the restore. After `DISPLAY_RESTORE_AFTER_PRESENTS` publishes, `qoh99_display_restore_tick` posts `WM_QOH99_APPLY_DISPLAY`, and `apply_display_restore` runs on the window message path. Never do this work inline: `SetWindowPos` inside a present re-enters the presenter through `WM_SIZE` and could charge a second slot to one frame. The startup 1x client must also not be saved as the player's size, so `build_windowed_restore_rect` computes the F11 return rectangle from the persisted values. The `[Window]` section mirrors the current state on every change, so a repeated `SetDisplayMode` re-applies the player's last choice.

**The low-level keyboard hook.** A `WH_KEYBOARD_LL` callback blocks all system keyboard input until it returns, and Windows silently removes a hook that overruns the LowLevelHooksTimeout setting. After that the hotkey is dead for the session. `LLKeyboardProc` therefore only posts `WM_QOH99_HOTKEY` and returns, and `WrapperWndProc` does the work. The hook is an optional convenience, since Wine, a restricted session or a compositor may refuse it or never deliver to it. `install_hotkey_hook` tries at most `HOTKEY_HOOK_MAX_ATTEMPTS` times, and the window procedure is the authoritative path. It handles F2 and F3 only when no hook is installed, otherwise one press would fire twice.

**Messages hidden from the game.** These are native facts. The game's window procedure rewrites the `WM_MOVING` rectangle back to the current position whenever its fullscreen flag is set, which makes the title bar undraggable, so `WrapperWndProc` keeps the move/size loop away from it. F1 is a window-message path that sets a latch, and the main loop answers it with a full cooperative-level, display-mode and surface re-creation. F1 is not read through the DirectInput buffer, so dropping the message is a complete fix and no key state needs clearing. The game also hides the cursor with `ShowCursor(FALSE)`, so `SetCursor` alone cannot bring it back. `apply_cursor_visibility` drives the display counter instead, with a bounded loop, because a wrong cursor is cosmetic and a spin is a hang. The game's error popups and font facename are CP932 bytes, and `hook_locale_iat` converts them to the wide APIs. Each of those two patches stands alone, and one failing does not undo the other.

**DPI.** `enable_dpi_awareness` makes the process per-monitor aware before the window exists. Otherwise Windows virtualizes the manifest-less game on scaled displays and the picture differs per machine despite identical files.

#### Process lifetime and the two startup patches

`DllMain` runs under the loader lock, where file, module, thread, D3D and lock work can deadlock, more so under Wine. Attach and detach do almost nothing, and every real entry point calls `wrapper_lazy_init`. A second thread arriving during init waits a bounded time, then continues, because a slow init must never become a hang. `wrapper_release_runtime` runs from `WM_DESTROY`, or when the presenting `IDirectDraw` is released after its window has gone. The second condition is narrow on purpose: the game creates and releases a throwaway `IDirectDraw` just to read the display mode, and a display re-init releases the main one while the window lives on. Tearing down at those points would remove the hook and the overlay device for the rest of the session.

The one exception to the attach rule is `qoh99_neutralize_december_startup_gate`. The December executable refuses to start unless `System\WinMes\Tir_Mes1.Img` has one exact file modification time, a check that compares metadata rather than content. Copying, extracting or restoring the game changes that time, and the player sees an error box naming a message file. WinMain runs the check before any DirectDraw call, so process attach is the last moment that is early enough. The patch writes NOPs over the 26-byte gate only after comparing every byte, leaving the same state as the November build, which has NOPs there. Nothing is allocated or logged at that point. The outcome is latched and `qoh99_report_december_startup_gate` logs it during lazy init.

**Full colour.** This is a native fact: the game renders its effects only when bit 0 of the active profile's flags is set, and otherwise never runs that code. Only the separate `CONFIG.EXE` writes the setting, so an install that never ran it shows no effects and no way to find out why. In `System\QOHcnf.key` the flags dword sits at `4 + 764 * index + 376`, and `CONFIG.EXE` accepts files of 8044 or 7732 bytes. `ensureWrapperDisplayConfig` in `src/client/Main.cpp` patches the 8044-byte form before the process exists, touching only the active profile, since a player may keep other profiles at 8-bit. `qoh99_force_full_color_config` in the wrapper is the fallback for a launch that bypasses the caster, and it takes effect only on the next launch, because the game reads the file before `DirectDrawCreate`.

Do not call `qoh99_force_full_color_live_profile`, which nothing calls. At `DirectDrawCreate` the live-profile pointer does not yet point at a profile, and its target can still be committed and writable, so its guard passes and the write lands in unrelated data. The game then dies with an InitAppMain load error. A writable address is not proof of what it holds. `validQohActiveConfigPointer` shows the check such a write would need, and it would also need a point after the config load.

#### Diagnosing "an effect doesn't draw"

Check the full-colour bit first; the launch and wrapper log lines say what was changed. After that, a native effect drawn through `GetDC`, `Ellipse` and `ReleaseDC` needs an exact RGB565 surface (`ddw_gdi_surface_format_supported`). Anything else returns `DDERR_UNSUPPORTED`, which is legal DirectDraw, so the game treats it as nothing worth reporting. The first few refusals are logged with the surface format at the normal level, and `qoh99_log_dc_summary` prints `unsupportedFormatRejects` without verbose logging.

#### Wrapper logging

`ddraw_log.h` logging is compiled into every build and decided at run time, so a stock package can produce a support log. `QOH99_WRAPPER_LOG` selects off (`0`, `off`, `false`, `no`), verbose (`2`, `verbose`, `trace`), or normal for any other value. Output is `ddraw_wrapper.log` in `QOH99_NETPLAY_SESSION_DIR` when that is set, otherwise beside the wrapper, and it is opened for append because several processes share one session directory. Repeats are collapsed, a lines-per-second ceiling reports what it dropped, and `WLOG_CAP_BYTES` and `WLOG_CAP_BYTES_VERBOSE` cap the file so it cannot fill a player's disk. None of it may run in `DllMain`. `QOH99_DDRAW_FORCE_LOGGING` is a CMake option for a one-off diagnostic DLL, for someone who launches from a shortcut with no way to set a variable. It is off by default.

**Import patching.** All patches go through `qplat::PeImage`, the one bounded parser shared with the DLL and the launcher. `ddraw_pe_shim.cpp` adds only C linkage, a rollback transaction so a multi-slot install restores what it wrote if a later slot fails, and one policy: a slot whose target already lies inside the host image is refused, because that is a forwarder, a delay-load stub or another tool's trampoline. The shim tells "not imported" from "refused" by matching the parser's `was not found` text, so changing that message breaks the distinction.

**Memory-diff instrument.** `qoh99_instrument.c` runs only when `QOH99_DDRAW_INSTRUMENT` is exactly `1`. It keys match edges on the scene state, because the game's in-match flag is set only in netplay and stays 0 in local matches.

#### Movie player details

Everything in `qoh99_movie.cpp` runs on the game thread: the graph is built and controlled from the MCI calls, and completion arrives at a message-only window, so no interface crosses an apartment. The game initializes COM itself, so `S_FALSE` and `RPC_E_CHANGED_MODE` are both normal, and `s_comInitialized` ensures only a call the wrapper made is balanced.

`RenderFile` reports success for a partial render, which is the black picture with music on a machine with no decoder. `s_graphHasVideo` is what decides a candidate worked, trying the game's file through Indeo, then the lossless copy, then the game's file as it is. The game's `OPENING.AVI` differs by edition (Indeo 5 or WMV3), and MCI's AVI device decodes only through Video for Windows codecs, which current Windows does not register for either.

`registerIndeoDriver` has three non-obvious constraints:

- It must use `ICINSTALL_DRIVER`, because the function-pointer form crashes the driver at its first open.
- It passes the 8.3 short path, because msvfw32's driver lookup stops at the first space.
- It must also `ICOpen` each candidate, because a successful `ICInstall` proves only that the path was recorded.

The game asks for a window handle right after opening and keeps it, and `IVideoWindow` exposes none, so the wrapper owns a child window and reports that. The game places the movie in its fixed 640x480 space, so the wrapper finds where that frame lands using the same letterbox rule as gameplay, then places the movie proportionally inside it. The intro therefore occupies the same rectangle as gameplay, with the same bars, and a resize re-applies the placement.

#### Overlay device and drawing

`CreateDevice`, `Reset`, `TestCooperativeLevel` and the final `Release` belong to the focus-window thread, as D3D9 requires. Requesters publish the newest desired state and post `WM_QOH99_D3D_DEVICE_WORK`. The `kDeviceInFrame` state stops an ImGui callback or a synchronous `WM_SIZE` from resetting a device under an open scene, and the worker re-posts until the frame ends.

- **Retry timing.** A hard creation failure backs off for `kDeviceRetryBackoffMs`. A device that is merely lost retries after `kDeviceLostRetryMs`, otherwise the overlay disappears for seconds after every Alt-Tab.
- **Resizes.** `kResizeCoalesceMs` limits `Reset` during a drag, while a single change such as F11 still resets at once.
- **Consumers.** Every invalidate and recreate goes through `NotifyDeviceObjectsInvalid` and `NotifyDeviceObjectsValid`, latched by `g_deviceObjectsLive`. A phase can never be sent twice or lost, so a failed `Reset` cannot strand a consumer in the invalidated state. A climbing rebuild count while the window is untouched indicates a lifecycle fault; the numbers are on the Diagnostics tab.

**Layering.** `WrapperOverlayDrawList` picks the ImGui list for everything drawn without a window: the HUD, the generic overlay, the input viewer and the replay readout. It chooses a layer and must never become a visibility gate. ImGui renders the background list first, then windows, then the foreground list. The game frame is not a list at all: `RenderFrameQuad` issues it as raw D3D9 earlier in the same scene, so every list composites over the match. With the menu open, overlays stay above the match and the menu sits above them. If overlays and the menu vanish together, look for a missing present (see held frames above) rather than a layering fault.

**Input viewer.** `input_display_layout.h` holds the geometry as pure arithmetic so it can be tested. The band is exactly 14 cells tall at 4:3 with no slack, so the gap above the history comes out of the history, and a row count that disagrees would drop a row with no other symptom. The scale is its own, not the HUD's `GameScale`, whose clamp exists to keep text readable and would shrink the strip at 4K. The strip is anchored to the image rather than the client rectangle, so it stays off the pillarbox.

The renderer holds no history. The producer folds once per confirmed simulation step and publishes the whole ring, so repeated presents, pauses and frame-step holds cannot advance it. Do not add a last-good-frame cache here: a ring outliving its session is the one way this overlay could paint a finished match's inputs over live content. A dropped seqlock read costs one invisible frame. The wrapper cannot link `qinput::inputDisplaySnapshotRenderable`, so it re-proves the same checks with its own copy, and the two must change together.

The live box and history entry 0 are the same input by contract (`inputDisplayLaneAppend` writes both), which is why they are separated by a gap and not only styled differently. Arrows are vectors because the default ImGui atlas is ASCII only, and a font or sprite dependency would fail silently under a Wine prefix. Training shows one lane, the one the human drives, so the strip jumping sides during a macro recording is the deliberate cue.

**Replay transport keys.** `EFZDX9_Presenter_SubmitReplayHotkey` keeps no state. Each press polls the runtime, passes back that poll's generation, and submits once. A press racing a session change is refused by the runtime, and a refused or dropped submit is not retried, because a resend would turn one press into two when the ring drained.

**Palette edits.** `UploadFrame` applies `PaletteApplyLiveOverride` on every present, because the game only re-runs its own palette copy at a scene change, so editing its tables shows nothing while paused. `g_palettePresentDiag` lets the Palette tab tell four outcomes apart, all of which look like "nothing happened" on screen: the frames are not 8-bit indexed so colours were baked in earlier; they are indexed with no palette attached; the override did not fire; or it fired and is writing.

**Menu navigation.** The DLL submits from the game thread and the menu drains on the presenting thread. `g_menuNavigationCount` keeps a per-control count beside the `g_menuNavigationPending` bitmask, because two presses between frames would otherwise merge into one bit. The drain takes one per frame so a burst walks the cursor row by row, and `kMenuNavigationMaxPending` saturates the count so a stalled presenter cannot replay a long backlog.

**Dead tabs.** `DrawSessionTab`, `DrawSettingsTab` and `DrawTestingTab` have no callers. They read through `GetModApi`, which looks for a module and exports that do not exist in this product, so wiring one in would only display "API unavailable".

**Finding the frame commit.** `ddw_is_qoh99_frame_commit` identifies a completed frame by its source: a full 320x240 read of the back buffer, onto the primary. Composition draws offscreen, and the KO zoom magnifies a sub-rectangle, so neither matches. The destination size is deliberately not part of the test, because the game recomputes its destination from the window's client area on every present. Requiring particular sizes would make other window sizes invisible, giving a black or frozen picture while the simulation runs on. The primary's own dimensions are checked, because they come from the display mode and not the window. Pixel format stays with the surface throughout: RGB565, RGB555 and A1R5G5B5 all occupy two bytes, so bytes per pixel can never select a format. `ddw_gdi_bridge_acquire` serves `GetDC` through a 32-bit scratch DIB and accepts only exact RGB565. `ddw_gdi_bridge_release` commits the pixels back only after the DC restore and `GdiFlush` have succeeded.

**Presenter ownership.** `g_deviceState` is the barrier between the game thread and the device work, which runs on the window's thread. `EFZDX9_Presenter_Present` claims it with a compare-exchange for the whole frame, so no Reset or Release can happen under an open ImGui frame. Whenever it cannot claim the state, it returns false and the frame goes out through GDI, so a device rebuild costs quality and never a frame. Ordering rules at the top of that function:

- `g_presentDepth` declines a nested present first. A window restyle inside the menu delivers `WM_SIZE` synchronously and repaints through this function while the outer frame is still open.
- `FlushPendingMenuCapture` runs before the device check. Closing the menu releases Training's hold on the player's controls, so that handshake cannot wait for a device that may never return.
- `SampleTrainingActionStateLog` runs next because this is the one point reached once per real frame on every exit path, including GDI fallbacks.

Resize requests are coalesced by `kResizeCoalesceMs`, since a drag delivers a new size on every mouse move. `EFZDX9_Presenter_ToggleMenu` flips `menuVisible` unconditionally, because that flag is what asks for a device in the offline case, and gating it on a ready device would leave Insert dead. Only the capture handshake is deferred while device work runs. `g_inputResyncPending` clears ImGui's input after a device handover, because a release may have been dropped during it, which would leave a phantom held key.

**The in-game training menu.** `DrawNativeTrainingMenu` is a second view over the same `Qoh99TrainingStatusV1` and the same command path as the ImGui menu, so the two have no separate state to disagree about. Rows are rebuilt from status every frame, and the row cursor is never saved, because a remembered index could point at a different control. It returns false if it has no font or game rectangle, and the caller falls through to the ImGui menu rather than show nothing. Outside English it uses the atlas font, because the pixel font has no Japanese glyphs. Rules a new row has to respect:

- **Value mapping.** Guard rows map to values rather than indices, because `QOH99_TRAINING_GUARD_AUTO` is deliberately absent from the list, so an index sent as a value would select the wrong mode. A runtime value missing from the list selects nothing rather than showing the first entry.
- **Slot selectors.** These send the choice in `slotOrSetting`, not `value`, because that is where the runtime reads a slot.
- **Row tags.** Menu-local rows use tags such as `kNativeMenuStyleRowTag`, and the commit handler tests the ranges in sequence. An overlap would make one row silently do another's job, so `static_assert`s pin each range against the next. Extend them when adding a range.

**Menu input.** Keyboard input comes from `NativeMenuReadInput`, which polls `GetAsyncKeyState` and not ImGui. It reads only while the menu is visible and the game window is in front, because that call reports the physical keyboard and would otherwise act on typing in another program. Bit 0 of the result matters: it latches a press until the next call, so a tap between polls is still seen, and the 8 ms poll limit (`kPollIntervalMs`) loses nothing. That holds only because this is the one reader of these keys. There is no auto-repeat, outside the palette editor's colour channels.

Controllers arrive through `EFZDX9_Presenter_SubmitMenuNavigation`, resolved by the DLL from the player's own bindings. The wrapper cannot see pads, and ImGui's gamepad navigation stays off because it would read XInput and bypass every binding. A keyboard press can therefore be reported twice on different clocks: by the poll at present rate, and later through the fight-input path when the game thread runs. A per-frame fix cannot remove the duplicate, so `DrawNativeTrainingMenu` keeps a debt in each direction (`debtAgainstSubmitted`, `debtAgainstOwnEdge`). Whichever reporter is first takes the step and cancels exactly one report from the other, and the debt expires after `kMenuDuplicateFrames`, so a later real press is never eaten. Mouse hover counts only while `mouseEngaged` is set, which a move or click turns on and a key press turns off. A resting pointer would otherwise re-select its row every frame.

### Native model (`qoh99_re/`)

`qoh99_re_model` is a standalone static library describing the game's own code: addresses, functions, build profiles, scene and match logic, and the state regions that make up a savestate. Addresses registered in the model live here, but the netplay code is not free of them: `src/GameHooks.cpp` and `ddraw_wrapper/qoh99_movie.cpp` also define address constants of their own.

**Source of truth.** `qoh99_re/model/qoh99_manifest.toml` is the input, and `qoh99_re/tools/generate_model.py` generates the headers in `qoh99_re/include/qoh99/generated/` plus a provenance lock in `qoh99_re/generated/model/manifest.lock.json`.

- With `--check` the generator writes nothing and fails on any drift. The build runs this as `qoh99_re_manifest_check` when `QOH99_RE_BUILD_TESTS` is on and Python is available.
- `validate_source_coverage` requires the hand-written `qoh99_re/include/qoh99/AddressMap.hpp` to match the manifest exactly, so an address has to be added in both places.
- `expected_counts` in the manifest catches accidental additions and removals.
- `Confidence` in `qoh99_re/include/qoh99/Confidence.hpp` records how well a fact is established, and `RegionInfo` carries that level with its evidence string.

**Consumers.**

- `src/RuntimeProfileRegistry.h` turns the build profiles into runtime decisions such as `profileAllowsHookInstall`.
- `src/rollback/SavestateProfile.hpp` derives its regions from `GameplayRegionCatalog`, so a region added to the model reaches rollback through one path.
- `src/qoh99/qoh99_addresses.h` adapts the constants for the hooks.

Tests run as `qoh99_re_unit` and `qoh99_re_manifest`.

**Which address constants are safe.** `fnaddr` in `qoh99_re/include/qoh99/AddressMap.hpp` is the December column, so a bare function constant says nothing about which build is running. That is only harmless for a function with the same address in both builds. The function constants in `src/qoh99_game.h` are all `NativeShiftClass::IdentityLayout`, which holds at or below `kIdentityLayoutLastEntryVa`. A `ShiftedEntry` function, such as `NativeSymbolId::ReadLogicalInput`, must never become a bare `constexpr`, because it would be the wrong address on November. Check the shift class in `NativeSymbolCatalog.hpp` before adding one. Callers go through helpers such as `fn::readLogical` and `fn::applyLogical` in `src/qoh99/qoh99_functions.h`, which resolve against the selected profile and return null when no profile is active.

Some entries exist to record a refusal. `NativeSymbolId::MessagePump` is `NativeRuntimeUse::NotCallable`, because hooking the native message pump crashes with a zero instruction pointer.

**The formation row.** `kVsFormationRow` holds the applied VS formation, and its aliases `kTeamSize` and `kMatchMode` are misleading: it is neither a team size nor a mode flag. As `VsFormation` records, 0 means no VS formation has been applied (the Arcade and Story state), 1 means two fighters (VS rows 0 and 1), and 2 means four fighters (VS rows 2 to 5). Code that tests for a 1v1 should write `<= 1u`, as `src/GameHooks.cpp` does, and treat 0 as a real third value.

**Native facts that shape the code.** These are facts about the game. The model's types are the place to read them.

- **Arena.** Match state alternates between two blocks (`ArenaFrame`). The game sets the active pointer before it flips the selector, so the two are one flip out of phase. Read `kArenaActivePtr` or save both blocks, and never derive the active block from `kArenaSelector`. The render-entity list inside a block is rebuilt every frame from emitters and HUD feeders, so differences in it during resimulation are cosmetic and not a desync.
- **Scene nesting.** There are three levels: `Scene`, then `Scene3Sub` for VS setup, then `MatchSub` for a match. `Scene::OpeningMovie` is the movie player, and the game has no recorded-match format of its own. `MatchSub` values 1 to 4 are not mapped, which the `Unknown` names state.
- **Formation.** `kVsFormationRow` is a formation code, not a team size. 1 is two fighters, 2 is four fighters at once, and 0 means no VS row applied, which is the Arcade and story state. A boot that leaves 0 selects the Arcade state machine: the round loser becomes CPU (`MatchFlowModel::cpuMaskAfterGameEnd`), the post-game handler drives the match substate itself instead of yielding (`postGameHandlerYieldsToSceneDispatcher`), and the launch gate can start on one selector (`launchGateFires`). `DirectBootController::supportsRow` accepts two-fighter rows only, rather than half-driving a four-fighter frontend.
- **Life.** Effective life is current plus reserve minus pending damage, in `kBattleLifeArray`. `kHealthArray` is misnamed: it is the separate regenerating guard gauge.
- **RNG.** `RngSystem` has a gameplay seed and a cosmetic seed behind a selector, so effects cannot disturb simulation draws. Its state is exactly those three words. The tick count is used once, to derive the initial seed.
- **Actors.** Every fighter-shaped object is 0x5FC bytes. Changes seen past that offset belong to the adjacent pool entry, so the struct must not grow. The input history is a separate object reached through a pointer in the fighter (`CommandHistory.hpp`), so it is restored by re-reading that pointer after the fixed regions.
- **Input facing.** A fighter's facing swaps the horizontal pair of the logical mask.

Model tests run through `BufferMemoryView`, and `LiveMemoryView` guards each live access with `VirtualQuery`, so the same system classes run host-side and in-process.

**How the December column stays honest.** `native_symbols` gives each symbol both builds' addresses, and `generate_model.py` recomputes the December one from `shift_class` and refuses the manifest on a mismatch. `IdentityLayout` and `DataUnchanged` mean the addresses are equal, and `ShiftedEntry` means a fixed 32-byte shift. That shift is a rule about function entries only: an edited function grew, so an address inside a function moves by an amount that is neither uniform nor 32. `InteriorOfIdentityFunction` and `SemanticallyEdited` therefore require a note, and the latter has both columns authored, because the generator will not infer them. Matching entry bytes do not prove matching behaviour, as `editedContentGate` shows. `runtime_use` answers whether a symbol is reachable today, and any `RawLiteral` value in it marks a place that bypasses the model.

**Qualifying a build takes two files.** `RUNTIME_QUALIFIED_BUILDS` is pinned in the generator and not read from the row it validates, and `runtime_qualification` must agree in both directions. Editing the manifest alone therefore fails. This set only governs hook install: a build also needs a qualified content manifest before an installation is admitted.

**Native game facts.**

- **Pacing is outside the tick.** Frame pacing lives in `WinMain`'s loop, so calling the match tick in a loop advances the simulation at full speed. That is what makes fast-forward possible.
- **Do not hook the message pump.** `kMessagePump` crashes when detoured, jumping to address zero.
- **`kVsFormationRow` is a formation.** It is not a match mode or a team size: 1 is two fighters and 2 is simultaneous four fighters. `VsFormationModel` captures this. Two confirmed selectors launch only the two-fighter rows, and the four-fighter rows wait for all four (`requiredSelectorConfirms`, `launchReady`). In character select, the confirm flag means "back to the VS list".
- **`kOpeningMovieFilename` selects the movie scene.** The game has no match-replay engine of its own, so a non-empty first byte there means the opening movie.
- **The game has its own network protocol.** `qoh99/VanillaNetProtocol.hpp` describes its DirectPlay messages, in `MsgType`. The project replaces the transport underneath them.
- **Contact policy order.** The game tries parry before the guarded path where the Faultless detector runs, so training must keep that order. `damageCriticalScaler` is the seam every damage caller reaches, so critical policy belongs there rather than in the ordinary hit path. The whitelists and divisors match in both builds.

### Wine and Proton

`scripts/launch_wine.sh` checks the environment, then starts `qoh99caster.exe`. Support on real Linux hardware has not been verified, and the script's own text says a pass cannot prove rendering, controller, audio or speed behaviour.

| Flag | Effect |
|---|---|
| `--check` | Run the checks and exit |
| `--recheck` | Ignore the stored marker and check again before launching |
| `--game-dir <path>` | Folder holding `qoh99.exe`; defaults to the script's folder |
| `--strict` | Refuse to launch if any check fails |
| `--skip-preflight` | Launch without checking at all |
| `--window`, `--in-terminal` | Choose where the caster draws |
| `--no-clear`, `--wine-stderr` | Keep the screen, or keep Wine's own error output |
| `--wine`, `--prefix` | Choose the Wine binary and `WINEPREFIX` |
| `--relay host[:port]` | Also check that a relay is reachable |
| `--verbose` | More detail from the checks |

**Non-fatal by default.** Without `--strict`, failed checks only warn and the launch continues, because the caster is the authority and refuses on its own with a precise reason. This matters for the build check: the script compares the raw executable hash against the one November value in `SUPPORTED_GAME_SHA256`. It recognizes the December hash in `OTHER_GAME_SHA256` only to report it as unsupported, so a December install is reported as failing and still launches unless `--strict` is given. The same applies to any executable the caster admits through `kAttestedVariants`.

**The override is the key detail.** `merge_ddraw_override` forces `ddraw=n,b` so Wine loads the packaged wrapper rather than its builtin, while keeping unrelated `WINEDLLOVERRIDES` rules and replacing any existing `ddraw` rule. `mergeDdrawOverride` in `src/platform/CompatEnv.h` applies the same rule to the game process the caster spawns. `detectCompat` reports Wine and Proton from `ntdll` exports and the environment.

**Marker and console.** Passing checks write `.qoh99-preflight` beside the game, keyed on the package manifest, the game's size and date, the Wine version and the directory. Changing any of these re-runs the checks. The script picks `wineconsole` when a display is available and sets `QOH99_CONSOLE_VT` to match. Wine's stderr goes to `logs/wine-stderr.log` by default, because it would otherwise draw over the caster's menu.

**Why a console window is the default.** The caster draws by writing cells or by writing VT sequences, and the two environments need opposite ones. `wineconsole` has a real screen buffer but does not interpret VT, and a Unix terminal understands VT but has no buffer to poke. A wrong guess gives a mangled menu or a screen of raw escape codes, so `launch_wine.sh` sets `QOH99_CONSOLE_VT` rather than leaving the caster to guess. Wine's approximate buffer in a terminal also truncates rows and leaves stale text, which is why the window is preferred. Attach `logs/wine-stderr.log` first in a bug report.

**What the checks are.** The script checks what only Linux can see: Wine version and prefix, filesystem case behaviour, path length against the game's ANSI loader, the effective overrides, the display route, ports, and an in-Wine process, injection and IPC test (`--wine-fixture`). Its package and build checks repeat what the caster verifies and exist only to catch a bad unzip early. It does not change the prefix or install anything, and the marker is its only write. The marker key also includes `SCRIPT_VERSION`, so changed checks re-run. A marker can only be trusted because every input to the answer is part of its key. A missing runtime file is a failure, while `PACKAGE_DOC_FILES` only warn, because a text file cannot stop the game. The script uses `pipefail`, as most checks end in a pipeline that would otherwise hide a failing producer.

### Portability plumbing

These live in `src/platform/` and exist so a Wine prefix degrades rather than breaks.

- **Child environment.** `mergeDdrawOverride` keeps every raw entry, including the special "=X:" drive entries in each environment block, because dropping those changes the child's per-drive current directories.
- **Optional providers.** `loadOptionalModule` loads DbgHelp, BCrypt and WinINet at run time, so a prefix without a working copy starts anyway.
- **Paths.** `src/platform/WidePath.h` is the single UTF-16 path layer, so a deep Proton prefix or a non-ANSI user name cannot truncate a path. The conversions return empty on invalid input, because a path that cannot be represented must not become a different path. `isDirWritableW` tests create, write, rename and delete, since a mount can allow creation while `atomicReplaceFileW` still fails.
- **State directory.** `stateRootW` tries `QOH99_NETPLAY_STATE_DIR`, then the executable's folder, then `QOH99Netplay` under `LOCALAPPDATA`. It evaluates once per process. `originalNameFromTrashLeafW` sits beside `moveToTrashDirW` so that a quarantined file's name can always be recovered.
- **Sockets.** Wine may leave the socket type and protocol unset in the structure `WSADuplicateSocketW` returns. `repairDuplicatedUdpProtocolInfo` fills only unset fields, because the project transfers only IPv4 UDP sockets.
- **Finding `LoadLibraryW` in a suspended game.** A process created suspended has only `ntdll` mapped and an empty module list, so enumeration cannot succeed and retrying is pointless. `remoteModuleBaseOrValidatedCandidate` tries enumeration first, then the caster's own load address for that module, which Windows normally shares per boot. It reads the image at that address from the target and requires the expected machine, size and export before using it. A whole-image read would fail, because a mapped system image has unreadable pages, so only the needed ranges are read.
- **Support bundles.** `writeSessionPreamble` gives each component the same first log block: build, compatibility layer, state directory and modules. It takes no cookies, peer addresses, credentials or nicknames, so a bundle can be shared.

## Reference: formats, layouts and native facts

Offsets and sizes are in bytes. Every format here is little-endian and written field by field, with no struct serialized, unless the table says otherwise. Native addresses are not listed here. They live in `qoh99_re/include/qoh99/AddressMap.hpp`, `qoh99_re/model/qoh99_manifest.toml` and `qoh99_re/include/qoh99/generated/NativeSymbolCatalog.hpp`, and the symbol names below come from those files where one exists.

### Replay file: V2 (`src/replay/ReplayCodecV2.cpp`, `src/client/replay/ReplayWriterV2.cpp`)

The prefix and chunk header have the same shape as V1 below, with these differences.

| Offset | Field in V1 | Field in V2 |
|---|---|---|
| Prefix 4, 6, 8 | Version fields from the prefix struct | `kContainerVersionMajorV2` (2), `kContainerVersionMinorV2` (0), `kContainerReaderMajorV2` (2) |
| Prefix 20, 28 | `sessionUuidLow`, `sessionUuidHigh` | `gameUuidLow`, `gameUuidHigh` |
| Chunk 16 | `matchId` | `gameGeneration` |
| Chunk 20 | `firstTimelineFrame` | `firstRecord` |
| Chunk 24 | `lastTimelineFrame` | `nextRecord` |

V2 simulation contract, `kSimulationContractBytesV2` = 144:

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | `executableSha256` |
| 32 | 8 | `nativeBuildIdentity` |
| 40 | 8 | `runtimeSimulationId` |
| 48 | 8 | `stateRegionSchemaFingerprint` |
| 56 | 8 | `stateHashProjectionId` |
| 64 | 8 | `stateHashAlgorithmId` |
| 72 | 32 | `contentManifestRoot` |
| 104 | 8 | `bootstrapSchemaId` |
| 112 | 8 | `eventSchemaId` |
| 120 | 8 | `characterCatalogId` |
| 128 | 8 | `terminalResultSchemaId` |
| 136 | 4 | `compactActionDomainVersion` |
| 140 | 4 | Reserved, zero |

### Replay file: V1 codec (`src/replay/ReplayCodec.cpp`, `src/client/replay/ReplayWriter.cpp`)

Prefix, `kPrefixBytes` = 48:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `Q99R` |
| 4 | 2 | `containerMajor` | |
| 6 | 2 | `containerMinor` | |
| 8 | 2 | `minimumReaderMajor` | |
| 10 | 2 | `prefixBytes` | Always `kPrefixBytes` |
| 12 | 4 | `endianMarker` | `0x04030201` |
| 16 | 4 | `featureFlags` | |
| 20 | 8 | `sessionUuidLow` | |
| 28 | 8 | `sessionUuidHigh` | |
| 36 | 8 | Reserved | Zero |
| 44 | 4 | `prefixCrc32c` | CRC32C of the 48 bytes with this field zeroed |

Chunk header, `kChunkHeaderBytes` = 48:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `syncWord` | `CHNK` |
| 4 | 2 | `kind` | |
| 6 | 2 | `schemaVersion` | |
| 8 | 1 | `critical` | |
| 9 | 1 | `codec` | |
| 10 | 2 | Reserved | Zero |
| 12 | 4 | `sequence` | |
| 16 | 4 | `matchId` | |
| 20 | 4 | `firstTimelineFrame` | |
| 24 | 4 | `lastTimelineFrame` | |
| 28 | 4 | `recordCount` | |
| 32 | 4 | `payloadBytes` | |
| 36 | 4 | `decodedBytes` | |
| 40 | 4 | `payloadCrc32c` | |
| 44 | 4 | `headerCrc32c` | CRC32C of the 48 bytes with this field zeroed |

Simulation contract, `kSimulationContractBytes` = 128:

| Offset | Size | Field |
|---|---|---|
| 0 | 32 | `executableSha256` |
| 32 | 8 | `runtimeSimulationId` |
| 40 | 8 | `stateRegionFingerprint` |
| 48 | 8 | `stateHashProjectionId` |
| 56 | 8 | `stateHashAlgorithmId` |
| 64 | 8 | `bootstrapCodecVersion` |
| 72 | 8 | `eventSchemaVersion` |
| 80 | 32 | `contentManifestRoot` |
| 112 | 8 | `characterCatalogId` |
| 120 | 4 | `fpuControlWord` |
| 124 | 4 | `mxcsr` |

Session manifest. A string is a u16 length plus UTF-8 bytes, so fields after the first string have no fixed offset.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 128 | `SimulationContract` | |
| 128 | 1 | `bootstrapKind` | |
| 129 | 1 | `vsRow` | |
| 130 | 1 | `cpuMask` | |
| 131 | 1 | `cpuContract` | |
| 132 | 4 | `slotToSourceRoute` | One byte per slot |
| 136 | var | `lane[0..3]` | Each: active u8, then a string |
| .. | 32 | `slot[0..3]` | 8 bytes each (`kFighterSlotDescriptorBytes`) |
| .. | 4 | `rosterEpoch` | |
| .. | 8 | `recordedUnixSeconds` | Signed |
| .. | 4 | `timezoneOffsetMinutes` | Signed |
| .. | 1 | `sourceKind` | |
| .. | 1 | `networkMetadataPresent` | |
| .. | 2 | `playerOneDelay` | |
| .. | 2 | `playerTwoDelay` | |
| .. | 2 | `rollbackWindow` | |
| .. | 4 | `averageRttMicros` | |
| .. | 4 | `lossPermille` | |
| .. | var | `routeLabel` | String |
| .. | var | `title` | String |
| .. | 4 | Reserved | Zero |

Input block:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `firstTimelineFrame` | |
| 4 | 4 | Reserved | Zero |
| 8 | 90 each | `record[0..count-1]` | `kInputRecordBytes`; record N is frame `firstTimelineFrame + N` |

Footer index, `kFooterFixedBytes` = 56 plus the entries:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `chunkCount` | |
| 4 | 4 | `matchCount` | Also the number of index entries |
| 8 | 4 | `totalFrames` | |
| 12 | 8 | `finalRollingDigest` | |
| 20 | 4 | Reserved | Zero |
| 24 | 56 each | `entry[0..n-1]` | `kMatchIndexEntryBytes`; strictly increasing ordinal and offset |
| .. | 32 | `fileDigest` | |

`FooterIndex::fileDigest` (checked in `src/client/replay/ReplayParser.cpp`) is SHA-256 over these, in exact file order:

1. `prefix[0 .. kPrefixBytes)`.
2. For every chunk before the footer chunk, `header[0 .. kChunkHeaderBytes)` then `payload[0 .. payloadBytes)`.
3. The footer chunk header, with `payloadCrc32c` and `headerCrc32c` (bytes 40..47) replaced by zero.
4. The footer payload `[0 .. payloadBytes - 32)`.

### Replay events (`src/replay/ReplayCodec.cpp`, `src/spectator/SpectatorSetArchive.h`)

The two event types share numeric kinds (`EventKind` in `src/replay/ReplayTypes.h`, `SetEventKind`), but the set archive adds `PaletteSelected` and its `NetworkSample` differs. In the replay event, unlisted fields are reserved and writers emit zero. In the set archive, `isValidSetEvent` enforces the columns as shown.

| Kind | `ReplayEvent` (`valueA`/`B`/`C`) | `SetEvent` (`valueA`/`B`/`C`) |
|---|---|---|
| `RosterSelected` | Not scalar | Only `roster[]`, `selectorProof[]`, `selectorProofMask`, `rosterEpoch`; A, B, C are zero |
| `StageSelected` | C = native stage id | C = stage id |
| `MatchStarted` | C = match ordinal within the set | C = match ordinal, 1-based |
| `RoundStarted` | C = round number, 1-based | C = round ordinal, 1-based |
| `RoundEnded` | A = winning side mask (bit N = slot N), at most `0x0F`; C = round number | A = winning side mask, at most `0x0F`; C = round |
| `MatchResult` | A = winning side mask, at most `0x0F`; B = round wins, low byte side 0, next byte side 1 | Same; B must fit in 16 bits |
| `RematchRequested` | C = requesting slot | C = requesting slot, below 4 |
| `ReturnedToCharacterSelect` | Reserved | All scalars zero |
| `RouteEpochChanged` | A = byte N source lane for slot N; B = byte N `SlotOwnership` for slot N; C = new route epoch | A = packed slot-to-source route; B = packed ownership; C = new route epoch, nonzero |
| `SessionPaused` / `SessionResumed` | C = requesting slot | C = requesting slot, below 4 |
| `DelayChanged` | A = P1 delay, B = P2 delay, C = rollback window, in frames | Same; each at most 120 |
| `NetworkSample` | A = average RTT us, B = jitter us, C = loss permille | A = average RTT us, B = loss permille (at most 1000), C = zero |
| `WallClockPause` | A = wall-clock microseconds stalled outside simulation time | A = wall-clock microseconds paused |
| `PaletteSelected` | Not a replay kind | C = palette index, at most `kMaxPaletteIndex`; `activeSlotMask` has one bit; `paletteEntries` holds one colour variant |
| `SegmentTerminal` | `terminalReason`, `stateProof` | `terminalReason`, `stateProof` = post-tail state; A, B, C are zero |

Every set event other than `RosterSelected` must carry `selectorProofMask == 0`, four inactive identities with zero proofs, and `rosterEpoch == 0`, or the digest changes. One colour variant is sixteen B, G, R, X entries (`kSetPaletteEntryBytes` = 64).

### Replay rolling digest (`src/replay/ReplayIntegrity.cpp`)

`digest(n) = mix(digest(n-1), canonical scalar encoding of record n)`. `mix` is FNV-1a/64 (offset basis `0xCBF29CE484222325`, prime `0x100000001B3`), one little-endian byte at a time, then the splitmix64 finalizer (`avalanche`). `rollingDigest` is the output and is never mixed in.

| Record | Tag | Encoding order |
|---|---|---|
| Frame | `0x01` | previous, tag, `timelineFrame`, `inputDomain`, `activeLaneMask`, `actions[0..3]`, `routeEpoch`, `preStateHash`, `postStateHash`, `lifecycleBefore`, `lifecycleAfter`, `dispatcherResult` |
| Event | `0x02` | previous, tag, `timelineFrame`, `kind`, `matchId`, `roster[slot]{nativeCharacterId, paletteIndex}`, `selectorProof[slot]{cursorCell, alternateFlag, confirmState}`, `selectorProofMask`, `rosterEpoch`, `valueA`, `valueB`, `valueC`, `terminalReason`, `stateProof` |

A lifecycle is `{scene, requestedScene, sceneThreeState, matchState, roundState, layoutGeneration}`.

`src/game/StateProofDigest.h` values are compared byte for byte by peers and recordings. They use only unsigned integer arithmetic and `memcpy` loads, never floating point, CPU feature detection, intrinsics, or anything derived from an address.

### Replay checkpoint (`src/replay/ReplayCheckpoint.cpp`)

The order is a little-endian header with its CRC32C, the regions split into chunks with a CRC32C each, the relocations with their own CRC32C, then SHA-256 over everything before it. No pointer-bearing savestate buffer may be copied into `.q99r`.

| Part | Size | Contents |
|---|---|---|
| Header | 200 (`kCheckpointHeaderBytes`) | Magic, schema version, reserved, `regionCatalogDigest`, `timelineFrame`, `recordCursor`, `eventCursor`, `preStateHash`, `rollingDigest`, `bootstrapFingerprint`, contract, decoded total, region count, relocation count, header CRC |
| Region header | 20 (`kCheckpointRegionHeaderBytes`) | `regionId`, `byteLength`, `hash` (u64), chunk count |
| Region chunk | 8 + data, at most 65536 data bytes | Length, CRC32C, data |
| Relocation | 24 each | `regionId`, `byteOffset`, `ownerKind`, `ownerIndex`, `targetRegionId`, `targetIndex` |
| Relocation CRC | 4 | CRC32C of all relocations |
| Whole digest | 32 | SHA-256 |

### Replay IPC (`src/replay/ReplayIpcWire.h`, `src/game/replay/ReplayIpcWorker.h`, `src/client/replay/ReplayBroker.h`)

Frame header, `kReplayIpcHeaderBytes` = 44, no padding. A frame is 44 + `payloadBytes` long.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `kReplayIpcMagic`, `QRP1` |
| 4 | 2 | `wireVersion` | `kReplayIpcWireVersion` |
| 6 | 1 | `channel` | 1 recorder, 2 broker |
| 7 | 1 | `type` | `MessageType` |
| 8 | 4 | `generation` | `replayChannelGeneration` or `recordChannelGeneration` for this launch |
| 12 | 8 | `nonce` | `replayBrokerNonce` for this launch |
| 20 | 4 | `sequence` | Monotonic, +1 per message per direction |
| 24 | 4 | `param0` | Per type; never a length or an offset |
| 28 | 4 | `param1` | Per type |
| 32 | 4 | `payloadBytes` | At most `kReplayIpcMaxPayloadBytes` |
| 36 | 4 | `payloadCrc32c` | CRC32C of the payload; 0 when empty |
| 40 | 4 | `headerCrc32c` | CRC32C of bytes `[0, 40)` |
| 44 | var | payload | Exactly `payloadBytes` |

`ipc::v3` in the same header defines a 64-byte frame header (`QRP3`):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic |
| 4 | 2 | `kWireVersion` (3) |
| 6 | 1 | `channel` (1 recorder, 2 playback) |
| 7 | 1 | `type` |
| 8 | 4 | `launchGeneration` |
| 12 | 4 | `gameGeneration` |
| 16 | 8 | `gameUuidLow` |
| 24 | 8 | `gameUuidHigh` |
| 32 | 8 | `launchNonce` |
| 40 | 4 | `sequence` |
| 44 | 4 | `param0` |
| 48 | 4 | `param1` |
| 52 | 4 | `payloadBytes` |
| 56 | 4 | `payloadCrc32c` |
| 60 | 4 | Header CRC over `[0, 60)` |

`Hello` and `RecorderHello` payload, `kHelloPayloadBytes` = 28:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `nonce` | Echo of `qipc::IpcConfig::replayBrokerNonce` |
| 8 | 4 | `gamePid` | |
| 12 | 2 | `wireVersion` | |
| 14 | 2 | `featureBits` | |
| 16 | 8 | `manifestDigest` | `qipc::IpcConfig::replayManifestDigest` |
| 24 | 4 | Reserved | Zero |

`SessionInfo` payload, `kSessionInfoBytes` = 88:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `manifestDigest` |
| 8 | 8 | `sessionUuidLow` |
| 16 | 8 | `sessionUuidHigh` |
| 24 | 8 | `simulationContractId` |
| 32 | 8 | `contentDigest` |
| 40 | 4 | `matchId` |
| 44 | 4 | `matchOrdinal` |
| 48 | 4 | `firstTimelineFrame` |
| 52 | 4 | `lastTimelineFrame` |
| 56 | 4 | `totalFrames` |
| 60 | 4 | `matchCount` |
| 64 | 1 | `bootstrapKind` |
| 65 | 1 | `vsRow` |
| 66 | 1 | `cpuMask` |
| 67 | 1 | `activeLaneMask` |
| 68 | 1 | `activeSlotMask` |
| 69 | 1 | Reserved, zero |
| 70 | 4 | `slotToSourceRoute` |
| 74 | 2 | Reserved, zero |
| 76 | 8 | `bootstrapFingerprint` |
| 84 | 4 | Reserved, zero |

`RecorderManifest` payload:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 48 | `ReplayPrefix` | `encodePrefix` form |
| 48 | 4 | `manifestBytes` | |
| 52 | var | Manifest | Exactly `manifestBytes`, `encodeSessionManifest` form |

`Terminal` and `RecorderTerminal` payload, `kTerminalPayloadBytes` = 24:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 2 | `reason` | `qreplay::TerminalReason` |
| 2 | 2 | Reserved | Zero |
| 4 | 4 | `lastTimelineFrame` | |
| 8 | 8 | `postTailStateHash` | |
| 16 | 8 | `rollingDigest` | |

Game-thread rules:

- `src/game/replay/ReplayIpcWorker.h`: the game thread never touches a pipe, socket, file, worker-held mutex or worker-owned allocation. It uses only preallocated bounded SPSC rings.
- `src/game/replay/ReplayRuntime.h`: `replayNextFrame()`, `replayInjectedAction()`, `replayCommitFrame()`, `replayPlanOuterSlot()` and `replayRecordConfirmed()` run on the game thread, with no allocation, contended lock, wait, or file, pipe or socket call.

### Replay annotations and battle log (`src/client/replay/ReplayAnnotations.cpp`, `src/client/replay/ReplayBattleLog.cpp`)

Annotation store header:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `RAN1` |
| 4 | 4 | version | |
| 8 | 4 | `entryCount` | |
| 12 | 4 | `bodyCrc32c` | Over everything after byte 16 |
| 16 | var | Entries | |

Annotation entry, in order:

| Size | Field |
|---|---|
| 8 | `sessionUuidLow` |
| 8 | `sessionUuidHigh` |
| 32 | `fileDigest` |
| 1 | `flags`: `0x01` favorite, `0x02` structural, `0x04` simulation, `0x08` provenance |
| 3 | Reserved |
| 8 | `structuralVerifiedUnix` |
| 8 | `simulationVerifiedUnix` |
| 32 | `verifiedContentRoot` |
| 8 | `verifiedRuntimeSimulationId` |
| 8 | `sourceSessionUuidLow` |
| 8 | `sourceSessionUuidHigh` |
| 32 | `sourceDigest` |
| var | `title`, u16-prefixed |
| var | `note`, u16-prefixed |
| var | `sourceFileName`, u16-prefixed |
| 1 | Tag count |
| var | Tags, u16-prefixed each |

`battles.q99log` is UTF-8, LF-terminated and TAB-separated. Field 0 is a record-kind token (`G`) and field 1 the schema version (`kSchemaVersion`). Free text is last and has no escaping. Readers take known fields by index and ignore extra trailing fields.

### Spectator publication (`src/game/spectator/SpectatorPublisher.h`, `src/client/spectator/SpectatorSidecar.h`)

The game side and the sidecar each hold these constants, and they must match. Frame header, 36 bytes:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `QCPS` (`0x53504351`) |
| 4 | 2 | `schemaVersion` | |
| 6 | 2 | `kind` | `PublicationWireKind` / `SidecarMessageKind` |
| 8 | 4 | `sequence` | Per connection, from 1 |
| 12 | 4 | `param0` | Per kind, see below |
| 16 | 4 | `payloadBytes` | |
| 20 | 4 | `payloadCrc32c` | |
| 24 | 8 | `gameId` | |
| 32 | 4 | `headerCrc32c` | CRC32C of bytes `[0, 32)` |

Payloads:

| Kind | `param0` | Payload |
|---|---|---|
| `Hello` (1) | | `gameId` u64, `schemaVersion` u32, reserved u32 |
| `Start` (2) | | `gameId` u64, `simulationContract` u64, `initialStateHash` u64, `initialDigest` u64, `firstTimelineFrame` u32, P1 delay u16, P2 delay u16, `rollbackWindow` u16, reserved u16, metadata length u32, metadata |
| `Confirmed` (3) | Record count | Records of 32 bytes: `record` u32, `timelineFrame` u32, P1 u16, P2 u16, reserved u32 (zero), `stateHash` u64, `rollingDigest` u64 |
| `Checkpoint` (4) | Record cursor | `recordCursor` u32, `byteLength` u32, bytes |
| `Seal` (5) | Reason | `reason` u16, reserved u16, exclusive record cursor u32, prefix digest u64, `postTailStateHash` u64 |
| `TailProof` (6) | Record cursor | Cursor u32, reserved u32, `postTailStateHash` u64 |
| `PublicationFault` (7) | `PublishDisableReason` | `PublishDisableReason` u16, reserved u16, accepted record cursor u32, prefix digest u64, `postTailStateHash` u64 |
| `ChainedSetEvent` (10) | 1 | A one-event `SetEventBatchPayload`, ordered by cursor among `Confirmed` batches |

`ConnectedDescriptorV4` (8) and `ConnectedPublicationV4` (9) are additive and defined in `src/spectator/SpectatorProtocolV4.cpp`.

### Spectator protocol values (`src/spectator/SpectatorProtocol.cpp`, `src/spectator/SpectatorGrant.cpp`, `src/spectator/TopologyGrant.cpp`)

Ready metadata, `kReadyMetadataBytes` = 224 (`QNP1`, version 5):

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | |
| 4 | 4 | version | |
| 8 | 4 | `seedGameplay` | |
| 12 | 4 | `seedCosmetic` | |
| 16 | 4 | `selector` | |
| 20 | 4 | `initialPhase` | `kReadyInitialPhase` |
| 24 | 16 | Contract literals | `10000`, `606`, `0x027F`, `0x1F80` |
| 40 | 8 | `bootstrapHash` | |
| 48 | 16 | Match config | `timerMode`, `aiDifficulty`, `lifeMultiplier`, `roundsToWinMinusOne` |
| 64 | 4 | `vsRow` | |
| 68 | 4 | `cpuMask` | |
| 72 | 4 | `slotToSourceRoute` | |
| 76 | 4 | Masks | `activeLaneMask`, `activeSlotMask`, `rosterEpochLow`, reserved |
| 80 | 16 | `playerNameCodePage[4]` | |
| 96 | 128 | `playerName[4][32]` | |

Spectator grant body, `kSpectatorGrantBodyBytes` = 56. Grants are bearer tokens, so redact them from logs. The shared viewer cookie must never be the grant MAC key. The MAC is HMAC-SHA256 truncated to 128 bits over this encoding.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `grantId` |
| 8 | 8 | `viewerId` |
| 16 | 8 | `sessionId` |
| 24 | 8 | `expiryUnix` |
| 32 | 8 | `resumeDigest` |
| 40 | 4 | `sourceGeneration` |
| 44 | 4 | `resumeRecord` |
| 48 | 1 | `route` |
| 49 | 7 | Reserved, zero |

Topology grant body, `kTopologyGrantBodyBytes` = 44. The MAC is the same construction, with a leading domain tag. The MAC key never travels toward a node, and these grants are also bearer tokens.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `kTopologyGrantMagic` |
| 4 | 2 | `kTopologyGrantVersion` |
| 6 | 2 | Reserved, zero |
| 8 | 8 | `grantId` |
| 16 | 8 | `nodeId` |
| 24 | 8 | `sessionId` |
| 32 | 8 | `expiryUnix` |
| 40 | 4 | `authorityGeneration` |

Playback seal (`spectatorSourceStopsOnDispatchReturn` in `src/game/SpectatorPlaybackPolicy.h`): a native dispatcher return of zero quits the process in scenes 1 and 3, and returns to character select in a match. The source seals only at the former boundary, without waiting or altering the original return value.

### Set-seek boot descriptor (`packSetSeekBoot` in `src/GameHooks.cpp`)

Checkpoint region 2, `kSetSeekBootBytes` = 40.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `kSetSeekBootMagic`, `QSSB` |
| 4 | 4 | version | 1 |
| 8 | 1 | `vsRow` | The packer writes 1 |
| 9 | 3 | Padding | |
| 12 | 5 per slot, 2 slots | Slot data | `char`, `palette`, `cell`, `alt`, `confirm` |
| 22 | 2 | Padding | |
| 24 | 4 | `logicalStageIndex` | |
| 28 | 4 | `openMatchId` | The packer writes `g_spectatorSetMatchOrdinal` |
| 32 | 4 | `highestMatchId` | Same value |
| 36 | 4 | `matchesSoFar` | Same value |

### Network formats (`src/net/Autopunch.cpp`, `src/net/Protocol.h`, `src/net/ControlLink.h`, `src/net/RelayProtocol.h`, `src/net/RouteCandidates.h`)

Punch service packets. Multi-byte fields are big-endian and IPv4 is copied in network byte order.

| Packet | Size | Layout |
|---|---|---|
| Register | 2 | `localPort` |
| Lookup | 8 | `localPort`, target IPv4 (4 bytes), `targetPort` |
| Mapping | 8 | `internalPort`, `natPort`, IPv4 (4 bytes) |
| Punch | 1 | `0x00` |

Control-link datagrams are `qnet::packMsg` frames with a 12-byte `MsgHeader`:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | `Q99N` |
| 4 | 1 | `type` | |
| 5 | 1 | `flags` | |
| 6 | 2 | `bodyLen` | |
| 8 | 4 | `bodyCrc` | CRC32 of the body |

`MSG_LINK_HELLO` and `MSG_LINK_HELLO_ACK` establish the link and travel raw. `MSG_REL_DATA`, `MSG_REL_ACK` and `MSG_KEEPALIVE` form the reliable channel.

Relay record families, selected by the 16-bit version field, each with a disjoint kind space:

| Constant | Value | Family |
|---|---|---|
| `kVersion` | 1 | Player lane: Hello, Welcome, Register, Ready |
| `kSpectatorVersion` | 3 | Per-viewer spectator lane |
| `kObservedVersion` | 3 | Relay-observed endpoint discovery |
| `kTopologyVersion` | 4 | Per-node topology control lane |

`SpectatorHello` field use by party and admission:

| Party / admission | `sourceToken` | Other fields |
|---|---|---|
| Source, `SpectatorAdmission::None` | `spectatorRegistrationToken` from the player Welcome | Every viewer field is zero |
| Viewer, `Grant` | Zero | The caster-issued per-viewer grant |
| Viewer, `PublicToken` | `viewerAccessToken` from the player Welcome | Grant, viewer, generation and MAC fields are zero. The relay binds the request to the current source generation and returns a fresh viewer identity and token |

Spectate address (`formatSpectateUri`). Version 2 is `kSpectateAddressVersion`.

```
qoh99://spectate/2?s=<session>&cp=<control-port>
                  &lan=<ip:port>[,<ip:port>...]
                  &pub=<ip:port>&relay=<ip:port>&rr=<32hex room>
                  &kt=<8hex>&rat=<8hex>[#k=<32hex>&ra=<32hex>]
```

`cp` appears only when nonzero. `lan`, `pub` and `relay` each appear only when the set carries that kind, and `rr`, `kt` and `rat` only when the address holds that value. Everything before `#` is safe to log.

### Opening movie (`src/game/OpeningMovieFrame.h`, `ddraw_wrapper/qoh99_movie.cpp`, `ddraw_wrapper/qoh99_movie_policy.c`)

A Q99V frame is a 16-byte header (`kQ99VHeaderSize`) followed by a zlib stream:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | magic | `Q99V` |
| 4 | 1 | version | 1 |
| 5 | 1 | `kind` | 0 key, 1 delta against the previous decoded frame |
| 6 | 1 | `predictor` | 0 none, 1 sub (left neighbour), 2 up (row above) |
| 7 | 1 | Reserved | |
| 8 | 4 | `sequence` | Frame index in the stream |
| 12 | 4 | `rawSize` | The yuv410p planes: `W*H + 2*(W/4)*(H/4)` |

Native movie seam:

- The probe routine (`QOH99_GAME_MOVIE_PROBE_ADDR`) looks for the AVI and fills a path buffer (`QOH99_GAME_MOVIE_FILENAME_ADDR`). A zero first byte in that buffer means no movie.
- Before any patch is written, the probe's 11-byte prologue is verified (`QOH99_MOVIE_PROBE_PROLOGUE_BYTES`): `sub esp, 108h` (`81 EC 08 01 00 00`), then `mov eax, <global>` (`A1` plus a 4-byte operand). The exact bytes are in `ddraw_wrapper/qoh99_movie_policy.c`.
- The game opens the movie with `mciSendCommandA(0, MCI_OPEN, 0x30202, &parms)`, where `0x30202` is `MCI_WAIT | MCI_OPEN_ELEMENT | MCI_DGV_OPEN_WS | MCI_DGV_OPEN_PARENT`.

### DirectDraw vtables (`ddraw_wrapper/ddraw.c`)

Both tables follow the SDK order. `g_DDSurfVtbl` has the 36 `IDirectDrawSurface` entries plus 4 more through the Surface2 and Surface3 slots, because the game queries surfaces for `IDirectDrawSurface3` and receives the same object. `g_DDVtbl` has the 24 `IDirectDraw2` entries.

| Surface offset | Method | Notes |
|---|---|---|
| 8 | `Release` | |
| 20 | `Blt` | |
| 28 | `BltFast` | |
| 44 | `Flip` | Primary surface |
| 48 | `GetAttachedSurface` | |
| 100 | `Lock` | |
| 116 | `SetColorKey` | |
| 124 | `SetPalette` | |
| 128 | `Unlock` | |

| DirectDraw offset | Method | Notes |
|---|---|---|
| 8 | `Release` | |
| 16 | `CreateClipper` | Windowed mode |
| 20 | `CreatePalette` | |
| 24 | `CreateSurface` | Primary, offscreen buffer, sprite sheets |
| 48 | `GetDisplayMode` | Desktop bpp check at init |
| 76 | `RestoreDisplayMode` | |
| 80 | `SetCooperativeLevel` | 17 = fullscreen exclusive, 8 = normal |
| 84 | `SetDisplayMode` | 5-argument form, fullscreen only |
| 88 | `WaitForVerticalBlank` | |

### Sprite data read by the palette preview (`PaletteSpriteReadActor` in `ddraw_wrapper/render_dx9_imgui.cpp`)

| Actor offset | Points to or holds | Layout |
|---|---|---|
| +1432 | 64-byte frame record | +0 u32 composite index; +4 and +6 int16 sprite offsets; +21 render transform (`& 7`) |
| +8, +12 | Composite table and count | 12-byte entries: u32 `firstTile`, u32 `tileCount`, u16 `canvasW`, u16 `canvasH` (0 means 256) |
| +0 | Tile table | 260-byte entries: int16 x, int16 y, then 256 palette-index bytes (one 16x16 tile, pixels inline) |
| +170 | Facing | Left mirrors by XORing transform bit 2 |
| +175 | Worn colour variant | `16 * variant`, masked with `0x30` |

Colour is `palette table[variantByte + pixel]`, and pixel 0 is transparent.

### Training UI bridge (`ddraw_wrapper/render_dx9_imgui.h`)

The bridge is a fixed POD ABI called only from the presentation thread. The command callback may only attempt a nonblocking enqueue and must do no game work. `QOH99_TRAINING_UI_API_VERSION` changes only when a field changes meaning or a callback signature moves, never to add fields.

Size negotiation for `Qoh99TrainingStatusV1`:

1. It has a frozen prefix of `QOH99_TRAINING_STATUS_V1_BASE_SIZE` bytes, and new fields are only appended.
2. The caller passes its `sizeof` as `outSize`.
3. The provider writes exactly the prefix or the whole struct, and reports the count in `structSize`.
4. The caller accepts `structSize` in `[BASE_SIZE, its own sizeof]`, zero-fills the rest, and gates each appended group on `structSize` containing it.

Command fields:

| Command | `slotOrSetting` | `value` | `flags` |
|---|---|---|---|
| `REFILL_LIFE`, `REFILL_GUARD_GAUGE`, `REFILL_EX_METER` (20..22) | Zero | `Qoh99TrainingRefillTargetV1` | Zero |
| Kinds after those that take a slot | Target or policy slot | Setting | Per kind |
| `SET_TRIGGER_CONFIG` | `Qoh99TrainingTriggerEventV1` | Recording 0..8, or `QOH99_TRAINING_TRIGGER_MOVE_VALUE_FLAG` plus a one-based character move | Delay in the low 16 bits, chance in the high 16 |

The Reduce policy mirrors `qtraining::TrainingReducePolicy`, the mash-rate damage reduction. The verdict the hit resolver consumes is 2 (the hit lands at 70%), 1 (90%) or 0 (full damage). `reduceVerdictForPolicy` returns 2 for `AlwaysStrong`, 1 for `AlwaysLight`, 0 for `Never`, and the native verdict for `Native`.

### Native actor populations (`qoh99_re/include/qoh99/FighterPool.hpp`)

Every entry is a 0x5FC-byte object (`kFighterSize`).

| Population | Count | Symbols | Notes |
|---|---|---|---|
| Static fighters | 4 | `kFighterP1`..`kFighterP4` | 1v1 uses P1 and P2 only; the live table is `kFighterPtrTable` (4 entries) |
| Helper clones | 4 | `kHelperCloneP1`..`kHelperCloneP4` | Striker and assist |
| Shadow pool | 16 | `kShadowPool` | Afterimages |
| Effect pool | 30 | `kEffectPool` | The match loop updates all 30; some spawn helpers search only slots 0..19 |
| Actor clone pool | 120 | `kActorClonePool` | Spawned by `kClonePoolSpawn`; needs its own save region |

### Native scenes, formations and input (`qoh99_re/include/qoh99/SceneSystem.hpp`, `src/qoh99/qoh99_state_machine.h`, `src/game/DirectBoot.h`, `src/qoh99/qoh99_fields.h`, `src/input/NativeBindingTable.h`)

| State | Symbol | Values |
|---|---|---|
| Scene | `kSceneCurrent`, `kSceneNext` | 0 transition, 1 title and menu, 2 match, 3 VS setup, 4 opening movie, 255 exit |
| Scene-3 substate | `kScene3Substate` | 0 setup, 1 VS list, 2 character select, 3 confirm |
| Match substate | `kMatchSubstate` | See below |

| Match substate | `QohPhase` | Meaning |
|---|---|---|
| -1 | `CharaIntro` | Round setup; the recorder is not driving fighters |
| 0 | `RoundActive` | Gameplay |
| 1..4 | `RoundUnknownSub` | Unidentified native substates |
| 5 | `RoundKO` | Round win or KO; loops to -1 |

Rollback runs only in scene 2 with match substate 0.

Team topology is fixed and slot-based: fighter slots {0, 2} (P1 and P3) against {1, 3} (P2 and P4). A dual-character current/backup object swap stays inside one fighter slot and never renumbers a slot, a side or a source lane.

Formation globals (`src/game/DirectBoot.cpp`):

| Symbol | Meaning |
|---|---|
| `kMatchFormationRow` | Applied VS formation row: 0 none (Arcade or Story), 1 two-fighter (VS-list rows 0 and 1), 2 four-fighter or team (rows 2..5). It is not a game-mode flag, and 0 does not mean VS |
| `kVsFormationApplied` | Boolean: a VS formation has been applied |
| `kCpuSlotMask` | Bit n set means fighter slot n is CPU |

The launch commit (`NativeSymbolId::DirectBootApplyRoster`) commits the match from character select. It writes the resolved roster identity and palette bytes and sets the next scene to 2. Replay playback validates exactly those bytes.

Command and input-history object at fighter +164 (`kFi_ChildPtr`):

| Offset | Layout | Constant |
|---|---|---|
| +8 | Direction ring: 32 entries of 4 bytes, value = logical direction nibble shifted left 16 (mask `0xF0000`) | `kCmd_DirRing` |
| +136 | Direction hold counts: 32 entries of 4 bytes | `kCmd_DirHold` |
| +264 | Direction head index, 1 byte; advance `(h+1) & 0x1F` | `kCmd_DirHead` |
| +268 | Button ring: 16 entries of 4 bytes, value = button nibble (mask `0xF`) | `kCmd_BtnRing` |
| +332 | Button hold counts: 16 entries of 4 bytes | `kCmd_BtnHold` |
| +396 | Button head index, 1 byte; advance `(h+1) & 0xF` | `kCmd_BtnHead` |

Facing transform (`LogicalInput::withFacingSwap` in `qoh99_re/include/qoh99/InputSystem.hpp`): fighter +170 == 1 swaps left and right. The native order is exact. Right is tested first and both horizontal bits are cleared, so simultaneous left and right becomes left.

Native binding table: 4 players by 8 logical actions by a 12-byte descriptor (`kNativeBindingTableBytes` = 384). This is the same size as the configuration record's binding range below.

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `sourceType` | 0 keyboard (`codeOrMask` is a DIK scancode); 1..4 WinMM joystick id (`sourceType - 1`) |
| 4 | 4 | `codeOrMask` | |
| 8 | 4 | `callback` | |

### Native match configuration (`src/RollbackManager.h`, `src/netplay/MatchConfig.h`, `src/GameHooks.cpp`)

Selected configuration record, stride `kQohConfigRecordStride` (0x2FC):

| Offset | Meaning |
|---|---|
| +36 | Timer mode, 0..2 (`kMaxTimerMode`) |
| +40 | COM difficulty, 5 table entries (`kMaxAiDifficulty` = 4) |
| +44 | Life multiplier; base life is multiplied by value + 1, and only zero is supported (`kSupportedLifeMultiplier`) |
| +48 | Rounds to win minus one, 3 table entries (`kMaxRoundsToWinMinusOne` = 2) |
| +52, +56 | Background and HUD render modes |
| +60 | BGM backend: 0 Wave, 1 MIDI, 3 off, 4 CD |
| +64 | Local frame-pacing and clock mode |
| +68 | Screen-effect mode |
| +80 | Bit 0 `ExtraFunctionEnable`, bit 1 `SystemPause` |
| +84, +88, +92, +96 | Local pacing counters |
| +100..+112 | Display, window and audio-device modes |
| +116..+375 | Installation asset path |
| +376 | Renderer format flags |
| +380..+763 | Physical input bindings |

Stage identity (`src/replay/StageCatalog.cpp`): a logical stage index 0..27 maps through a 28-entry native table (`kSeStageCountV2`) to an applied background resource id 1..28.

Selector grid (`kSeGrid` in `src/replay/CharacterCatalog.cpp`, file offset `0x9EEE4` in the game executable), row-major 3 by 8, with column 8 hidden:

| | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 (hidden) |
|---|---|---|---|---|---|---|---|---|
| Row 1 | 00 | 01 | 02 | 03 | 04 | 05 | 06 | 1E |
| Row 2 | 07 | 08 | 09 | 0A | 0B | 0C | 0D | 20 |
| Row 3 | 0E | 0F | 10 | 11 | 12 | 13 | 14 | 21 |

### Native RNG (`qoh99_re/include/qoh99/RngSystem.hpp`, symbols `kRngDraw`, `kRngSrand`, `kRngGetGameplaySeed`)

| Operation | Behaviour |
|---|---|
| Draw | `seed = 156683941 * seed + 1`, then `(seed >> 15) & 0x7FFF` |
| Seed | Sets both seeds |
| Getter | Returns the gameplay seed |
| Select | Sets `kRngSelector`: 0 uses `kRngSeedGameplay`, nonzero uses `kRngSeedCosmetic` |
| Tick generator | `(273*x - 273*GetTickCount()) / 39`, the only non-deterministic source |

A savestate needs exactly 3 dwords: both seeds and the selector.

### Native message protocol (`qoh99_re/include/qoh99/VanillaNetProtocol.hpp`)

The game's DirectPlay messages dispatch on the leading u32 of the message.

| Type | Payload | Receiver effect | Rollback relevance (`rollbackRelevance`) |
|---|---|---|---|
| 0 `MSG_INPUT` | 12 bytes: type, packed input, 2 position-sync i16 words | Input ring (`kNetInputRing`, depth 3) | Input feed |
| 1 `MSG_SEQ` | Frame sequence | Stores field 1 | Sequence basis |
| 2 `MSG_SEED` | RNG seed | Seeds both RNG seeds | Seed sync |
| 3 `MSG_SIDE` | Side | Stores field 1 | Side |
| 4 `MSG_CHAT` | Chat text | Chat callback | None |
| 5 `MSG_PARAM5` | Parameter | Stores field 2 | Unknown |
| 6 `MSG_CONFIG` | 4232-byte match config | Config +40 and +48, a 20-byte block, and a 0x334-byte block at wire offset 3412 | Boot config |
| 7 `MSG_READY` | Ready | Sets the ready flag | Boot gate |

Bytes 32..3411 of type 6 are unmapped, and the 0x334 block is the wire tail.

### Native HUD, COM scripts and audio (`src/GameHooks.cpp`, `src/game/DeterministicAudioPolicy.h`)

- **Portrait and life draw (`QohPortraitLifeDrawFn`):** one fastcall per side, fighter in `ecx` and destination block in `edx`. The destination selects the on-screen slot (`kArenaActivePtr` target +87608 left, +87740 right) and is the reliable discriminator, because team mode alternates which fighter fills it.
- **Stateful HUD producers (`QohExDrawFn`, `QohComboDrawFn`):** the EX draw advances the EX spend transaction and the combo draw advances the combo display lifetime. They are hidden by the descriptor transaction, never by skipping.
- **Simulation-gating sounds (`kSimulationStatusSoundIds`):** these are the only DirectSound buffers whose `GetStatus` result feeds simulation control flow. They are 2, 4, 6 and 8 (character-select voice completion gates), 162 (round announcement), and 166..171 (the six round-result announcements). Every other sound is presentation-only and must not enlarge rollback state.

COM-AI bank table, `kAiBankCount` = 20 records of `kAiBankStride` (0x110):

| Offset | Size | Field |
|---|---|---|
| +0 | 0x104 | Bank path (`kAiBankPathBytes`) |
| +0x104 | 4 | Script pointer, `malloc(L+2)` (`kAiScriptPtrOffset`) |
| +0x108 | 4 | Metadata count (`kAiMetaCountOffset`) |
| +0x10C | 4 | Metadata pointer (`kAiMetaPtrOffset`) |

`.Fob` file layout:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `descriptorCount` |
| 4 | 36 each | Descriptors |
| .. | 4 | `scriptLength` (L) |
| .. | L | Script |

In memory, the last two of the L+2 bytes are zero sentinels. Opcodes write into the script, so all L+2 bytes are rollback state.

### Native character data used by Training (`src/training/TrainingGameIntegration.inl`)

Attack-record lookup:

```
actionEntry = ptr(actor + 16) + 24 * u32(actor + 48)
frameBase   = u32(actionEntry + 0)
attackBase  = u32(actionEntry + 16)
frame       = ptr(actor + 24) + 64 * (frameBase + u32(actor + 64))
recordStart = attackBase + u8(frame + 31)
recordCount = u8(frame + 32)
record(i)   = ptr(actor + 40) + 24 * (recordStart + i)
```

Frame descriptor, 64 bytes (`kTrainingChrFrameRecordSize`), and the fighter field the native frame advance writes from each:

| Descriptor offset | Fighter field | Meaning |
|---|---|---|
| 33 | +156 | Posture nibble |
| 14 (`& 0xF`) | +158 | Permission bits |
| 8 | +56 | Duration in ticks |
| 12 | +52 | Next frame index: `0xFFFF` advances by one, `0xFFFE` is terminal, any other value jumps to that local frame |

Stun durations by fighter +157, as frame lengths f0..f4 and their total:

| +157 | Ground guard | Total | Hitstun | Total |
|---|---|---|---|---|
| 0 | 3 3 4 4 2 | 16 | 3 3 5 4 3 | 18 |
| 1 | 3 4 6 5 2 | 20 | 3 4 7 5 3 | 22 |
| 2 | 3 4 9 5 2 | 23 | 3 4 10 5 3 | 25 |
| 3 | Total only | 10 | | |
| 4 | Total only | 1 | | |

## The repository, and how to change it

### The two sides of every feature

Most changes touch both sides of one process boundary, so learn this before the file list.

`qoh99caster.exe` (`src/client/`) is the launcher and menu program. It owns settings, connection setup, route selection, and the replay browser. It launches the game, injects `qoh99_netplay.dll`, and hands over one fixed struct, `IpcConfig`, with a duplicated UDP socket (`WSADuplicateSocketW` in `src/client/Launcher.cpp`).

`qoh99_netplay.dll` (`src/DllMain.cpp`, `src/GameHooks.cpp`) runs inside the game and owns the simulation, rollback, recording and publishing. `ddraw.dll` (`ddraw_wrapper/`) also runs inside the game and owns presentation, the overlay menus and the HUD. It shares memory contracts with the netplay DLL and mirrors their structs in `ddraw_wrapper/render_dx9_imgui.h`, pinned by `static_assert`.

Three things follow:

- Anything chosen in the caster must cross `IpcConfig`, which is versioned.
- Anything the overlay asks for crosses a lock-free queue (`TrainingCommandQueue`: the render thread produces, the game thread consumes, and neither may block). Anything it displays crosses an append-only status struct.
- The caster and DLL must come from the same build. `src/client/BuildIdentity.cpp` searches the DLL's bytes for `kBuildContractMarker` and refuses to launch if it is absent.

Pure rules live in small headers with `constexpr` functions, for example `src/net/PacingBudget.h` and `src/game/RuntimeDelayControl.h`. Tests exercise them without a game. Put new policy there and leave `GameHooks.cpp` to read game memory and call it.

### Repository map

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Every target and test, except those in the two subprojects below |
| `README.md`, `README_ja.md` | Player manual, shipped as `README.txt` and `README_ja.txt` |
| `assets/` | Bundled `ir50_32.dll` and `sounds/opponent_joined.wav` |
| `cmake/` | Distribution steps: `CleanDist.cmake`, `WriteManifest.cmake`, `VerifyDistribution.cmake` |
| `ddraw_wrapper/` | `ddraw.dll`: rendering, ImGui and native-style menus, HUD, movie playback; own `CMakeLists.txt` and `tests/` |
| `packaging/` | `defender_exclusions.cmd` |
| `qoh99_re/` | Model of the game binary: `model/qoh99_manifest.toml` is the source of truth, `tools/generate_model.py` produces `include/qoh99/generated/` and `generated/model/manifest.lock.json` |
| `scripts/` | `launch_wine.sh` |
| `third_party/` | SDL3 (hash-pinned), Dear ImGui, MinHook, miniz |
| `tools/` | `ContentIdentityDiagnostic.cpp`, `convert_opening_movie.py`, `make_translation_table.py` and its JSON inputs |
| `BUILD/` | Build output, git-ignored |

| `src/` | Contents |
|---|---|
| top level | `DllMain.cpp` (DLL entry, reads `IpcConfig`), `GameHooks.cpp` (hooks and netplay frame loop), `RollbackManager.cpp` (production save, restore, hash), `CrashDump.cpp`, `CrashContext.cpp`, `qoh99_game.h` (`saveRegions`), `RuntimeProfileRegistry.h` |
| `audio/` | Rollback-safe audio: timeline, reconciler, `QohAudioRuntime.cpp` |
| `client/` | Caster: `Main.cpp` (menus, connection flow, replay browser), `ConsoleUi.cpp`, `SdlUi.cpp`, `Launcher.cpp`, `BuildIdentity.cpp`, `PublicIp.cpp`, `AntivirusPreflight.cpp` |
| `client/replay/` | Caster side of replays: catalog, parser, writer, browser model, annotations |
| `client/spectator/` | Caster side of spectating: `SpectatorSidecar.cpp`, `SpectatorRoute.cpp`, topology authority and client |
| `compat/` | `CompatibilityIdentity.h`: what two peers must agree on, including `kStateSchemaCurrent` |
| `content/` | Game file identity and the generated allow-list `SimulationContentCatalog.generated.hpp` |
| `crash/`, `runtime/` | Tests only (`test_crash_context.cpp`, `test_runtime_profiles.cpp`) |
| `game/` | DLL-side policy headers, `DPlayTransport.cpp` (all gameplay UDP), `DirectBoot.cpp` |
| `game/replay/`, `game/spectator/` | In-game recording runtime; in-game publisher that feeds the sidecar |
| `i18n/` | `Tr.cpp` and generated `TrTableJa.inc` |
| `input/` | Bindings, profiles, compiler, device backends, mode controls |
| `ipc/` | `IpcConfig.h`, `BuildContract.h` |
| `net/` | Sockets, `ControlLink.h`, hole punching, port mapping, route selection, `PacingBudget.h`, control protocol `Protocol.h` |
| `netplay/` | `RollbackSession.cpp`, `NetplayCore.cpp`, gameplay wire format `WireProtocol.h`; live test drivers in `tests/` |
| `platform/` | Wide paths, PE parsing, Wine compatibility |
| `qoh99/` | Game-facing address, field and function headers |
| `relay/` | Self-run traffic relay, reached as `qoh99caster relay` |
| `replay/` | Format and playback core with no process dependencies: `ReplayTypes.h`, `ReplayCodecV2.cpp`, controllers |
| `rollback/` | Catalog-driven `SavestateProfile.cpp`, `SyncHash.cpp`, `RollbackRuntime.cpp`, testable without a game |
| `spectator/` | Spectator protocol, archives, sessions, grants, resilient distribution |
| `tests/` | `Tests.cpp` (caster self-tests), `LogTests.cpp`, `production/` (source and catalog pins for the DLL) |
| `training/` | Training logic, `TrainingTypes.h` (ABI), `TrainingGameIntegration.inl` (compiled into `GameHooks.cpp`) |

### Where would I change this

#### Add a menu entry

**Caster.** Menus are `con.menu(title, items, backLabel)` calls in `src/client/Main.cpp`, followed by a `switch` on the returned index. The top level is the `"Main menu"` call; `settingsMenu` and `offlineMenu` are typical children. The index is positional, so insert the item and its `case` together. `Diagnostics` is appended only when `g_devMode` is set, so it must stay last.

`Console::menu` translates through `trs` and `trAll`, so an English string with no table entry appears in English. To add one, edit the JSON input and regenerate `src/i18n/TrTableJa.inc` with `tools/make_translation_table.py`. The table is sorted by UTF-8 bytes because the lookup is a binary search with `strcmp`; hand-inserted rows can become unfindable.

A setting also needs a `Settings` member and lines in `Settings::load` and `Settings::save`, stored through `qui::config`. If the DLL needs the value, it also needs an `IpcConfig` field (see [Bump a protocol](#bump-a-protocol)).

**In game.** The overlay has two renderers in `ddraw_wrapper/render_dx9_imgui.cpp`, chosen by `[Overlay] MenuStyle`: ImGui tabs such as `DrawTrainingTab`, and `NativeMenuStyle`. A control missing from one is invisible to users of that style, so add it to both. Rows act through `EnqueueTrainingCommand`.

A new command needs a `TrainingCommandKind` value and a `validCommandPayload` case in `src/training/TrainingTypes.h` (the queue rejects a kind that function does not list), the mirror value in `Qoh99TrainingCommandKindV1` in `render_dx9_imgui.h`, and a case in `src/training/TrainingGameIntegration.inl`. New display data is appended to `TrainingStatusV1`, and the new size is added to `kTrainingStatusV1SizeTiers`. That list is append-only, and the mirror in `render_dx9_imgui.h` is pinned with `offsetof` checks in `src/tests/production/test_production_training.cpp`.

#### Change delay rules

Delay has four separate rule sets. Find the right one first.

| Rule | Where | Basis |
|---|---|---|
| Absolute limits | `kMaximumGameplayDelay`, `kMinimumGameplayRollback`, `kMaximumGameplayRollback` in `src/game/RuntimeDelayControl.h`; `kCapabilityDelay`, `kCapabilityRollback` in `src/net/PacingBudget.h` | Maximum rollback cannot exceed the savestate pool, `kPoolSlots` in `GameHooks.cpp` |
| Setup screen | `qnet::recommend`, `requiredCoverage`, `enforcedOwnDelay`, `enforcedOwnRollback` in `PacingBudget.h`; `RouteRecommendation` and `recommendationScreen` in `Main.cpp` | Route measurement plus a safety margin |
| In-match floor | `netRouteLiveFloorFrames`, `netMinimumLocalDelayForRoute` in `GameHooks.cpp` | Mean one-way time minus the peer's rollback |
| Menu and character select | `frontendDelayForRttMicros` in `src/game/FrontendDelayPolicy.h`, with its results pinned by `static_assert` | p95 RTT, never below gameplay delay |

The setup and in-match floors differ on purpose. The setup value is a cushion and the live floor is only the physical minimum, so a player can tune below the recommendation but cannot underbuffer the opponent.

An in-match change flows as follows:

1. A `ModeControlAction` edge (`InputDelayDecrease`, `InputDelayIncrease`) is enabled only when `runtimeDelayControlsAllowed` passes.
2. `adjustedGameplayDelay` computes the request, and a request below the floor is refused and logged.
3. `netRequestGameplayDelay` records the request, and `netApplyPhaseInputDelay` applies it through `tryChangeInputDelayAtConfirmedBoundary` in `src/netplay/RollbackSession.h`. If no boundary arrives within `kRuntimeDelayDrainTimeoutMs` (5 seconds), the request is dropped and the applied delay stays. Changing only at a confirmed boundary keeps both peers on one timeline, so never write `g_netDelay` from elsewhere.

There is deliberately no pre-connection tuning page. Values are chosen once on the measured screen and then adjusted in the match; `README.md` states this and a test checks the wording.

Tests are in `src/netplay/tests/test_pacing_budget.cpp` and `test_runtime_session_ui.cpp`.

#### Add a transport

Gameplay always runs as UDP datagrams on one socket. The caster picks the route; `src/game/DPlayTransport.cpp` calls `sendto` and `recvfrom` on what it was given and does not know how the route was found. That leaves three kinds of change:

- **A new way to reach a peer, still UDP.** Add to `CandidateKind` and `candidateBaseLabel` (`src/net/RouteCandidates.h`), and to `RouteLabel`, `resolveLabel` and `selectRoute` (`src/net/RouteSelection.h`). Then add `plainRouteText` in `Main.cpp`. Candidates travel in `RouteCandidateWire`, and `routeLabel` in `IpcConfig` mirrors `RouteLabel`, so this is a protocol bump. `validateCandidateSet` is the admission check for what a peer sends.
- **A new setup channel.** Implement `ControlLink` (`src/net/ControlLink.h`) as `UdpControlLink` does. `UdpControlLink` is the ordinary-play channel, and a listener can own its socket, so receive must work at timeout zero as well as when blocking.
- **A spectator route.** Add to `SpectateRequestRoute` in `src/net/Protocol.h`. Update the `kSpectatorTopologyRouteDirect` family in `IpcConfig.h`, whose mapping is asserted in `BuildContract.h`, and `labelForCandidate` in `src/client/spectator/SpectatorRoute.h`. `SidecarTransport` is the seam for faking it in tests.

Anything that is not a datagram socket means rewriting `DPlayTransport.cpp` and the socket handoff, because the game process only receives a duplicated socket.

#### Add a spectator feature

First decide whether the data must be in lockstep with the match.

- **Must arrive at an exact point in the stream.** Add a chained item, following `Palette` end to end:
  - `ConnectedItemKindV4` and `ConnectedPublicationKindV4` in `src/spectator/SpectatorProtocolV4.h`. `isKnownConnectedPublicationKindV4` has a hard-coded upper bound, so update it too.
  - Encode and decode beside `encodePaletteItem` and `decodePaletteItem` in `SpectatorProtocolV4.cpp`.
  - Archive handling in `SpectatorConnectedArchiveV4.cpp`.
  - The in-game publisher (`src/game/spectator/SpectatorPublisher.h`), which talks to the sidecar with `SidecarMessageKind`.
- **Fixed for the match and known at start.** Add a field to `ReadyMetadata` in `src/spectator/SpectatorProtocol.h`, bump `kReadyMetadataVersion`, and fill it in `GameHooks.cpp` around `encodeReadyMetadata`.
- **Viewer behaviour only.** Waits, timeouts and catch-up are in `src/game/SpectatorPlaybackPolicy.h`. The title-bar states are built in `DPlayTransport.cpp`. Who may watch is `AdmissionPolicy` and `sidecarSetAdmissionPolicy`, with its menu in `spectatorServingMenu`.

The sidecar (`src/client/spectator/SpectatorSidecar.cpp`) runs in the caster process, away from the game thread, so a slow viewer cannot stall players. Do not add a path where the publisher waits on a viewer. The live lane `qoh99_game_e2e_spectator_unavailable_does_not_harm_players` covers this.

Tests are `qoh99_spectator_stack`, `qoh99_spectator_production` and `src/client/spectator/tests/test_spectator_sidecar.cpp`.

#### Extend replay metadata

Choose the cheapest storage that fits:

- **Per-user notes** do not touch the replay. `ReplayAnnotationStore` in `src/client/replay/ReplayAnnotations.h` keys them by replay UUID.
- **A true or false fact** can be a new bit in `DisplayMetadataV2::flags`. Add it to `kKnownDisplayFlagsV2` in `src/replay/ReplayCodecV2.cpp`, or validation rejects the file.
- **A new field** changes the format. Add it to `DisplayMetadataV2` in `src/replay/ReplayTypes.h`, including `operator==`. Then update the validator, encoder and decoder in `ReplayCodecV2.cpp`; all three read `manifest.display`, and field order must match. Consider `kContainerVersionMajorV2`, which `BuildContract.h` pins, and keep existing files parsing.

Values are filled into `displayV2` in `src/game/replay/ReplayRuntime.cpp`. Browsing reads them in `src/client/replay/ReplayCatalog.cpp` and `ReplayBrowserModel.cpp`. `SpectatorReplaySaveAdapter.cpp` rewrites a copy of the manifest for spectator saves (it changes `sourceKind` and blanks `routeLabel`), so decide there what a viewer's copy keeps.

Tests are `src/replay/tests/test_replay_v2_contract.cpp` and `test_replay_codec.cpp`, both in `qoh99_replay_core`.

#### Add a training hotkey

1. Take a free value in `ModeControlAction` (`src/input/ModeControlContract.h`). Do not grow the enum: `modeControlBit` is `1ull << action`, so `Count` is pinned at 64. Unused values are 7 to 9, 15 to 19 and 24 to 39.
2. Add a row to `kModeControlDescriptors` with domain `ModeControlDomain::Training`, trigger, device policy, stable config key and display name, then update the `kModeControlDescriptorCount` assert. `modeControlsMenu`, the profile code and `InputCompiler.cpp` iterate this table, so the rebinding UI needs no further code. The compiler rejects one physical binding on overlapping controls.
3. Optionally add a default in `src/input/ModeControlPolicy.cpp`, which takes a DirectInput scan code. Check `trainingDikForbidden`, and remember the bindings must fit `kMaxModeControlBindings`.
4. Add a `TrainingAction` value before `Count` in `src/training/TrainingTypes.h`.
5. Map it with `mapHeld` in `GameHooks.cpp`.
6. Act on the edge inside `trainingApplyActionEdges` in `TrainingGameIntegration.inl`: `if (fired(qtraining::TrainingAction::...))`. Actions cross as held state, and edges are derived on the game thread.
7. Update the action tables in `src/training/tests/test_training_core.cpp` and the `TrainingAction` list that `test_production_training.cpp` scans for.

#### Add a savestate region

A region is a span of game memory saved, restored and possibly hashed every frame. A span left out does not fail cleanly: a load or rollback leaves that memory as it was. Live memory can then contradict saved flags, and peers can diverge.

1. Add the row in both `qoh99_re/model/qoh99_manifest.toml` (`regions`) and `qoh99_re/src/GameplayRegionCatalog.cpp`, then bump `regions` in `expected_counts`. The generator rejects any difference between the two. `relevance` drives everything downstream: in `SavestateProfile.cpp`, `DeterministicGameplay` rows are saved and gameplay-hashed, and `RenderCosmetic` rows are kept only if they do not overlap a gameplay region.
2. Regenerate with `QOH99_RE_EVIDENCE_ROOT` set:

   ```
   python qoh99_re/tools/generate_model.py
   ```

   Generation first checks size, SHA-256 and COFF timestamp of both executables against the manifest, and refuses to run without the root. `--check` without a root validates against `manifest.lock.json`. The `qoh99_re_manifest_check` target runs in every build, so stale output fails the build.
3. Update the pinned counts. They are deliberately redundant:
   - `kGameplayRegionCount`, twice, in `qoh99_re/tests/test_generated_model.cpp`
   - the total and per-relevance counts in `qoh99_re/tests/test_region_catalog.cpp`
   - `ProductionSavedRegionCountTracksExtendedCatalog` in `src/tests/production/test_production_regions.cpp`, which pins the `saveRegions` count
   - the `fixedBytes` bounds in `src/rollback/tests/test_savestate.cpp`, if the size is large
4. If the span holds actor records, register every slot with `addPool` in `src/RollbackManager.cpp`, as `rina_crash_actors` does. Otherwise peers hash process-local descriptor pointers raw and report a false desync. `kDescriptorRoots` is a different list: the roots that interior pointers resolve against.
5. If gameplay hash bytes changed, bump the state schema in `src/compat/CompatibilityIdentity.h`: add a new `compatibilityDomainId` constant, point `kStateSchemaCurrent` at it, and add the old one to `isSupersededStateSchema`. Peers with different schemas refuse to pair. Replays from a superseded schema stay playable but can never be byte-verified.

#### Bump a protocol

`src/ipc/BuildContract.h` is the checklist. It `static_assert`s each version below, so changing a constant stops the build until you edit the assert. Edit the matching tag in `QOH99_BUILD_CONTRACT_MARKER` at the same time: no check ties a tag to its constant, and an unchanged marker leaves the caster's byte search unable to tell the builds apart.

| Version | Constant | Declared in | Covers |
|---|---|---|---|
| IPC | `kIpcVersion`, `sizeof(IpcConfig)` | `src/ipc/IpcConfig.h` | Caster to DLL; `DllMain.cpp` rejects a mismatch |
| Control | `qnet::kProtocolVersion` | `src/net/Protocol.h` | Setup exchange |
| Gameplay wire | `qnetplay::wire::kVersion` | `src/netplay/WireProtocol.h` | In-match packets |
| Spectator | `qnetplay::spectator::kSpectatorVersion`, `kReadyMetadataVersion` | `src/spectator/SpectatorProtocol.h` | Viewer stream and start metadata |
| Relay | `qnet::relay::kSpectatorVersion` | `src/net/RelayProtocol.h` | Per-viewer spectator relay lane; running relays must be rebuilt too. `kVersion`, `kObservedVersion` and `kTopologyVersion` in the same file cover the other relay lanes and are not asserted here |
| Replay files | `kContainerVersionMajorV2`, `kContainerVersionMinorV2` | `src/replay/ReplayTypes.h` | `.q99r` container |
| Replay IPC | `kReplayPlaybackIpcSchemaVersion` | `src/ipc/IpcConfig.h` | Playback channel |
| Runtime | `kRuntimeCompatibilityId` | `src/ipc/IpcConfig.h` | Caster and DLL runtime pairing, the host's session parameters that a joiner checks, and the spectator start descriptor |

Any field added to `IpcConfig` changes its size, so the size assert and the `SIZE` tag move together. The marker is a compiled-in string, so the caster's byte search also catches a stale DLL. Bumping a version separates peers by design, so a bump is correct whenever old and new would misread each other.

### Building

Visual Studio with a 32-bit target is required, because the DLL loads into a 32-bit game. CMake 3.21 or newer is needed for `qoh99_re`, Python 3.11 or newer is needed for the model checks (`generate_model.py` imports `tomllib`), and Python 3 runs the live tests.

```
cmake -S . -B BUILD -A Win32
cmake --build BUILD --config Release
cmake --build BUILD --config Release --target qoh99_dist
```

`qoh99_dist` is Release-only and writes `dist/` and `qoh99-netplay-win32.zip` in the source root. It verifies the configuration first, then installs the `Qoh99Distribution` component, writes `manifest.sha256`, zips, and runs `cmake/VerifyDistribution.cmake` over the result, which includes checking that the runtimes are 32-bit PE images. It needs `QOH99_BUILD_DDRAW_WRAPPER` on. Configuring fails if `SDL3.dll` does not match `QOH99_SDL3_DLL_SHA256`.

| Cache option | Purpose |
|---|---|
| `QOH99_LIVE_GAME_EXE` | `qoh99.exe` in a complete install. Live lanes register only if it exists with `System/QOHcnf.key` beside it |
| `QOH99_ENABLE_LIVE_GAME_E2E` | Turns the live lanes off (default on) |
| `QOH99_AUTO_ROUTE_GAME_EXE` | Complete November install for `qoh99_auto_route_smoke` |
| `QOH99_NOVEMBER_GAME_ROOT`, `QOH99_DECEMBER_GAME_ROOT` | Complete installs for `qoh99_content_identity`, compiled in as definitions |
| `QOH99_LIVE_GAME_REMOTE_HOST`, `QOH99_LIVE_GAME_CONTROLLER_ADDRESS` | Register the two-machine lanes over ssh |
| `QOH99_LIVE_GAME_E2E_OUTPUT_ROOT` | Where live artifacts go |
| `QOH99_OPENING_AVI`, `QOH99_INDEO_DRIVER` | Optional movie files for the distribution |
| `QOH99_DEPLOY_ROOT` | Destination of the `qoh99_deploy` target |
| `QOH99_BUILD_LEGACY_RELAY_SHIM` | Builds a standalone `qoh99relay` (default off) |

Set `QOH99_RE_EVIDENCE_ROOT` in the environment to a directory holding the two pinned executables:

```
<root>/november/qoh99.exe
<root>/december/QOH99.EXE
```

`resolveRuntimeExecutableFixtures` in `src/netplay/tests/RuntimeExecutableFixtures.h` reads it. Without it, the fixture tests in `qoh99_network_p0` and `qoh99_runtime_profiles` fail with a message naming this layout, and `qoh99_re_manifest` skips with return code 77. Executables alone are not enough for the tests that need a complete install, listed in the table above.

**What the release gate protects.** The distribution is an exact allowlist, and `cmake/VerifyDistribution.cmake` treats every check as a hard failure. A PE32+ artifact could never load into the game, and under Wine it would appear as an unexplained launch abort, so the build catches it first. The verifier reads files as hex because `file(READ)` in text mode turns CRLF into LF on Windows and would hide a CRLF regression in the shell scripts. It splits the hex into whole bytes so a pair such as `40 d0` cannot match `0d`. A `QOH99_OPENING_AVI` must declare a `Q99V` stream, and an Indeo original is refused. A packaged file naming `qoh99relay` fails too, because `qoh99caster relay` is the service.

`cmake/WriteManifest.cmake` writes `manifest.sha256` for `sha256sum -c`, normalized to LF, since CRLF would make Linux look for `README.txt\r`. Its staging file must not be called `<output>.tmp`: `configure_file` writes that name itself, and the manifest would silently come out empty. `cmake/CleanDist.cmake` empties `dist/` rather than removing it, because on Windows any handle on the folder itself (an editor, a shell, Explorer) makes directory removal fail while the folder is already empty. The allowlist still catches anything left behind.

### Testing

List tests with `ctest --test-dir BUILD -N -C Release`, and run them with `ctest --test-dir BUILD -C Release`. The Release flag matters on a multi-config generator. A configure with no live game registers 91 tests. `qoh99_auto_route_smoke` fails intermittently (about one run in eight), and `qoh99_resilient_topology_paired_activation`, `qoh99_resilient_topology_client_relay_qsr_process` and the worker checks in `qoh99_network_p0` are timing-sensitive on a loaded machine, so rerun them alone before treating a failure as real.

| Family | Tests | Count |
|---|---|---|
| Core units | `qoh99_logging`, `qoh99_training_core`, `qoh99_opening_movie`, `qoh99_caster_selftests`, `qoh99_runtime_profiles`, `qoh99_network_p0`, `qoh99_crash_context`, `qoh99_replay_core`, `qoh99_content_identity` | 9 |
| Rollback and DLL pins | `qoh99_rollback_layer`, `qoh99_production_surface` | 2 |
| Soak | `qoh99_soak_quick`, deterministic network runs of a toy simulation and of the production stack; `qoh99_soak_full` and `qoh99_soak_nightly` are build targets | 1 |
| Network process smoke | `qoh99_local_autopunch_semantics`, `qoh99_live_e2e_lifecycle`, `qoh99_relay_live_smoke`, `qoh99_auto_route_smoke` | 4 |
| Spectator core and processes | `qoh99_spectator_stack`, `qoh99_spectator_production`, `qoh99_resilient_spectator`, its `_process`, `_local_process`, `_node_runtime`, `_qsp_local_process` variants, `qoh99_released_client_root_control`, `qoh99_client_root_qualification` | 9 |
| Spectator topology | `qoh99_topology_authority_signature` and the other `qoh99_resilient_topology_` tests outside the fault table | 10 |
| Topology fault table | `qoh99_resilient_topology_b10_01_credential_expired_before_qsr` to `qoh99_resilient_topology_b10_46_unexpected_relay_qsr_offer`: one per wrong-identity, truncated, replayed, retry or revocation case | 46 |
| Wrapper | `surface_ops`, `hidden_resource_fill`, `surface_gdi_bridge`, `movie_policy`, `presentation_policy`, `input_display_layout`, `replay_hud`, `pe_parser` | 8 |
| Game model | `qoh99_re_unit`, `qoh99_re_manifest` | 2 |

With `QOH99_LIVE_GAME_EXE` set, about 40 more lanes appear, labelled `live;e2e;game` and named `qoh99_game_e2e_*`, plus `qoh99_training_live_e2e` and `qoh99_agent_protocol`. The `qoh99_game_e2e_*` lanes drive two real game processes through `src/netplay/tests/live_game_e2e.py`, and `qoh99_training_live_e2e` drives one through `live_training_e2e.py`. Those lanes hold one resource lock, so they run one at a time. `qoh99_agent_protocol` tests the remote agent without a game and takes no lock.

For work that cannot reach netplay, skip them with `ctest --test-dir BUILD -C Release -E "e2e|soak|game_"`. Check the diff really leaves netplay untouched before relying on that.

`ddraw_wrapper/tests/window_restore_tests.ps1`, `ddraw_wrapper/tests/device_mode_transition_tests.ps1` and `src/game/tests/replay_native_rollover_tests.ps1` are not registered with CTest. Run them by hand.

#### Source-scanning tests read the code as text

Much of `src/tests/production/` does not call the code. It finds the project root from `__FILE__`, reads files with helpers like `readSource`, `readNetplayFile` and `readWorkspaceSource`, then asserts on the text. This pins hook ordering and wiring that cannot be exercised without a running game. `test_production_training.cpp` alone is over 9,000 lines of this.

`sourceSlice(source, beginMarker, endMarker)` cuts a span between two literal strings, typically `static bool trainingApplyActionEdges(` up to the next function's signature, and then asserts within it. As a result:

- **A behaviour-preserving move can fail tests.** Renaming a function, reordering functions, or moving code to another file can fail a test. Before moving code in `Main.cpp`, `GameHooks.cpp`, `TrainingGameIntegration.inl`, `DirectBoot.cpp` or `render_dx9_imgui.cpp`, grep the tests for the function's name.
- **Neighbours are load-bearing.** A function may exist with its name fixed only because it ends another function's slice; `DrawTrainingTab` is one, and a comment above it says so.
- **A negative check can pass vacuously.** `sourceSlice` returns an empty string when a marker is missing, and "this text is absent" is then true of nothing. Tests guard this with checks like `CHECK(!edges.empty())`. Add that guard to any new slice, or the test stops testing without failing.

When a scan fails after a legitimate move, update the markers and keep the assertion. The assertion usually records a real ordering rule.

### Microsoft Defender

Defender can flag or quarantine these binaries, because a program that injects a DLL into another process resembles malware to heuristics. This can happen to your own fresh build, and it shows up as missing outputs or as a test that cannot start its executable. Check Protection history before debugging the build, and consider excluding your `BUILD/` folder.

For players, `preflightAntivirus` in `src/client/AntivirusPreflight.cpp` reports problems at caster start, and the caster then tells the player to run `defender_exclusions.cmd` (`packaging/` in the source, the install folder in a package) as administrator. `launchDefenderExclusionHelper` in the same file has no caller. Run as Administrator, that script adds the install folder plus process exclusions for `qoh99caster.exe`, `qoh99_netplay.dll` and `ddraw.dll`. Passing `remove` undoes them.

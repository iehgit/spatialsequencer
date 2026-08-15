# MIDI Timing and Ordering

Spatial MIDI Graph Sequencer produces MIDI events only; it does not synthesize audio. The sections below define its MIDI timing and ordering.

## Musical time

One grid step is one sixteenth-note interval. An ordinary edge takes a number of sixteenth-note ticks equal to its Manhattan distance:

```text
ticks = abs(x2 - x1) + abs(y2 - y1)
sixteenth_duration = 60 seconds / BPM / 4
next_trigger = current_trigger + ticks * sixteenth_duration
```

The engine retains fractional musical pulse positions and maps them to nanosecond deadlines from a tempo epoch. Deadlines therefore do not depend on worker wake-up time, and per-step rounding cannot accumulate into drift between note events and MIDI Clock.

## Note On

When a musical node is triggered:

1. Its Note On messages are scheduled at the node's trigger deadline.
2. Every pitch in a multi-pitch node uses the same timestamp, velocity, MIDI channel, and gate timing.
3. A rest node emits no Note On message, but traversal continues normally.
4. A relay node emits no note messages.

The shared gate makes a multi-pitch node **paraphonic** from the sequencer's point of view: all of its pitches are triggered and released together rather than having independent note lengths.

When an incoming ordinary edge reaches a relay, the relay chooses one outgoing route and triggers its musical target immediately at that same deadline. Relay output edges therefore have zero logical MIDI duration.

## Note Off and the Release Gap

For a musical node with an outgoing ordinary edge of `N` sixteenth-note ticks, the **Release Gap** `R` is an integer from 0 through 4, measured in eighths of one sixteenth-note interval:

```text
note_off = trigger + (N - R/8) * one_sixteenth
next_note_on = trigger + N * one_sixteenth
```

The default is `R = 1`, so the previous note is released **1/8 of a sixteenth-note interval** before the next node is triggered. At 120 BPM that gap is approximately 15.6 ms.

The Release Gap provides a real interval between Note Off and the next Note On, allowing a synthesizer with an ADSR envelope to enter its Release phase before a repeated or following note is triggered. The receiving instrument controls the actual release time; the sequencer does not wait for the envelope to finish.

The current range is:

| Release Gap | Effect                                                 |
|-------------|--------------------------------------------------------|
| `0/8`       | No gap; Note Off and the next trigger share a deadline |
| `1/8`       | Default                                                |
| `2/8`       | Two eighths of a sixteenth-note interval               |
| `3/8`       | Three eighths of a sixteenth-note interval             |
| `4/8`       | Half of a sixteenth-note interval                      |

The setting is global. It changes the end of the current note, not the time of the next node.

A dead-end musical node is a special case: it is held for one sixteenth-note interval, then receives Note Off and playback finishes. A dead-end rest waits one sixteenth-note interval before playback finishes. A dead-end relay finishes immediately.

## Same-deadline ordering

The engine uses deterministic ordering when MIDI-relevant events share a musical deadline.

For a normal transition boundary the effective order is:

```text
Note Off -> Note On -> MIDI Timing Clock
```

With a zero Release Gap (`0/8`), the previous Note Off and next Note On have the same timestamp, but Note Off is dispatched first.

A multi-pitch Note Off or Note On is emitted as one group using one timestamp; the individual pitches are written in the node's stored pitch order.

A terminal Note Off that ends playback is treated specially: if it coincides with an outgoing MIDI Timing Clock pulse, that final Clock pulse may be emitted first, after which the terminal Note Off ends the sequence and MIDI Clock output is stopped.

## MIDI Clock output

MIDI Clock output uses the standard **24 PPQN (pulses per quarter note)**, which is six Clock messages per sequencer sixteenth-note interval.

When Clock output is enabled before playback starts:

1. MIDI Start (`0xFA`) is sent at the transport start time.
2. The start node is triggered at that same musical time.
3. The first MIDI Timing Clock (`0xF8`) is scheduled at the same time, after the node trigger.
4. Subsequent Clock pulses are scheduled from the absolute tempo timeline.

Clock output uses the same absolute timing model as note events. The ALSA backend maintains a stable mapping between `std::chrono::steady_clock` and the ALSA real-time queue clock, so events that share an engine deadline receive the same ALSA timestamp.

Enabling or disabling Clock output while playback is running is deferred to a sixteenth-note boundary. MIDI Clock input and MIDI Clock output are mutually exclusive.

Stopping or pausing running Clock output sends MIDI Stop (`0xFC`). Resuming paused playback that previously had active Clock output sends MIDI Continue (`0xFB`) and resumes the musical state from the shifted absolute timeline.

## MIDI Clock input

With external MIDI Clock enabled, the application follows incoming MIDI Timing Clock messages rather than advancing nodes from its internal tempo deadlines.

- Six incoming Clock pulses represent one sequencer sixteenth-note interval.
- Incoming Clock timestamps are converted by the ALSA input backend into the same monotonic time domain used by the engine.
- Recent Clock intervals are measured to update the displayed/estimated BPM.
- Fractional Release Gaps that fall between two incoming Clock pulses are converted into real-time deadlines using the measured external pulse period, so Note Off is not restricted to integer MIDI Clock pulses.
- MIDI Start can start stopped, externally clocked playback.
- MIDI Stop stops externally clocked playback.
- MIDI Continue received while already following external input does not independently change sequencer state.

When external Clock is enabled during internal playback, the engine waits for a six-pulse alignment interval before handing playback to the external pulse domain. Disabling external Clock during playback hands timing back to the internal clock on a sixteenth-note boundary.

If incoming Clock disappears for the watchdog interval, the application uses its internal clock. Active notes are released during that lost-clock recovery so a missing Clock source does not leave notes hanging.

## MIDI note entry

MIDI note entry is a separate, optional input path from MIDI Clock input. It uses its own ALSA client/port and a lightweight input worker, so receiving keyboard notes does not share or consume the event stream used for external Clock synchronization or MIDI output scheduling.

Only MIDI Note On messages with nonzero velocity on the configured note-input channel are considered. Note Off messages and Note On with velocity zero are ignored. When playback is exactly **Stopped** and a musical node is selected, the incoming note replaces that node's first pitch and sets the node velocity. Additional pitches in a paraphonic node are preserved, and the node's note/rest state is unchanged.

At all other times—including Running, Paused, Waiting for clock, and clock/error states—the received note is consumed without editing the project. Notes played during playback are not applied later after the transport stops.

By default the note-entry input attempts to use the same ALSA port selected for MIDI output. `--midi-note-input-device` can select a different source, and `--midi-note-input-channel` selects its channel independently from the MIDI output channel. If the input source is missing or unavailable, the rest of the application continues normally. F5 independently retries MIDI output and note input; when the note source is implicit, it is resolved again from the newly connected output. MIDI Clock input is controlled separately with `I`.

## Pause, stop, and note cleanup

Pausing releases currently active notes:

1. Pending ALSA queue events are cleared.
2. MIDI Clock output, if active, receives MIDI Stop.
3. Active pitches receive Note Off.
4. MIDI CC 123 (All Notes Off) is sent on the output channel.

On resume, the remaining internal deadlines are shifted by the duration of the pause. If the paused node was sounding, its pitches are triggered again at resume time with the node's velocity.

A normal stop also clears pending events, stops MIDI Clock output, sends Note Off for tracked active pitches, and sends CC 123. The same cleanup is used during shutdown and timing/device failures.

## ALSA scheduling and CPU behavior

The transport worker sleeps until the next relevant event deadline, MIDI input poll, watchdog deadline, command, or periodic heartbeat. Idle and normal-playback CPU use therefore stays low.

MIDI output is timestamped through an ALSA Sequencer queue. The backend calibrates the ALSA queue clock against the C++ monotonic clock and converts engine deadlines into ALSA real-time timestamps. Pending musical events remain in the engine until their deadline rather than being queued far in advance, allowing live tempo changes, pause/resume, graph edits, Release Gap changes, and clock-domain handoffs to rescale or cancel future events correctly.

# AI composition assistant (AI-01, Milestone J): plan

Status: **planned, not scheduled.** Work starts only after Tier 1 is finished and the Command
Engine (T1-02) covers note, pattern, chord and parameter edits. Nothing in this document is
implemented.

## Goal

An optional assistant that helps write songs, chords, drum patterns, melodies and synth sounds.
The user describes what they want in plain language ("a dark four-bar progression in A minor",
"busier hi-hats in the chorus", "make this pad brighter"). The assistant proposes edits, the
user auditions them, and the user accepts or rejects them. An accepted proposal is one Undo
step.

## Principles

- **Optional and off by default.** Alien Studio works fully without it. Nothing is sent over
  the network until the user turns the assistant on and adds a key.
- **Bring your own key.** Users enter their own Anthropic (Claude) and/or OpenAI API key. The
  project never ships keys or pays for usage.
- **Keys stay out of projects and the repository.** Keys are stored with Windows Credential
  Manager (DPAPI). They are never written to project files, logs or crash reports.
- **Users can see what is sent.** A "what will be sent" view shows the exact song summary
  before a request.
- **No new edit paths.** The assistant only calls named Command Engine commands (Execute, Undo,
  Redo, Batch, Preview). It never edits the document directly and never touches the audio
  thread.
- **Model output is untrusted input.** Every proposed command is checked and clamped before
  preview: pitch and velocity range, step counts, clip lengths, parameter ranges, track
  existence, and note limits. Invalid commands are refused and reported, never partly applied.
- **Real-time safe.** Network calls, JSON parsing and planning run on a background thread. The
  audio callback sees only the finished edit, the same way it sees a mouse edit.

## Architecture

```
Assistant panel (UI)
   -> request builder: compact song summary (tempo, meter, key/scale, chord track, sections,
      tracks, selected clip notes/steps, synth parameters)
   -> provider layer (background thread): Claude or OpenAI, chosen by the user
        tool definitions: write_chords, set_drum_pattern, write_notes,
        set_synth_params, set_section, explain (no changes)
   -> validator: schema + range checks + clamping, refuses on error
   -> Command Engine Batch in Preview mode -> audition (draft, not saved)
   -> Accept = one Undo step | Reject = discard
```

- **Provider layer.** One interface with two backends (Anthropic Messages API, OpenAI API).
  Both support tool calling with JSON schemas. Use HTTPS through JUCE (`juce::URL` /
  `WebInputStream`), with timeouts, cancellation and readable error messages for offline, bad
  key, rate limit and quota errors.
- **Explain mode.** The assistant analyses and suggests without changing anything:
  Analyze -> Explain -> Suggest -> Preview -> Apply -> Undo (see ROADMAP.md, Milestone J).
- **Shared musical context.** The request builder reads the same chord and scale context as
  INTEL-01, so the assistant and the built-in smart features agree.

## Increments (each about one PR)

1. **J1: Offline generators.** Deterministic, rule-based chord progressions in a key/scale
   and genre drum templates, exposed as Command Engine batches. Useful without AI, fully
   testable, and later callable by the model as tools.
2. **J2: Command validator.** A schema and range checker for proposed note, step, chord and
   parameter commands, with clamping and all-or-nothing refusal.
3. **J3: Song summary builder.** A compact, versioned text/JSON description of the project
   that fits a request budget, plus the "what will be sent" view.
4. **J4: Provider layer and key storage.** Claude and OpenAI backends behind one interface,
   background thread, cancellation, Credential Manager storage, settings page.
5. **J5: Assistant panel with preview.** Chat panel, draft audition, Accept/Reject, one Undo
   step, busy/offline states.
6. **J6: Chords and drums.** First assistant features, built on J1 to J5.
7. **J7: Melody and bassline.** Notes in the Piano Roll, respecting the chord track and scale.
8. **J8: Synth patch suggestions.** Parameter changes for the built-in synth from a text
   description ("warmer", "pluckier").
9. **J9: Arrangement help.** Section structure and variation suggestions (hardest; last).

## Testing

- Unit tests for the validator (out-of-range pitches, oversized patterns, unknown tracks,
  malformed JSON, partial tool calls), the summary builder and the offline generators.
- Provider tests use recorded responses (fixtures) so tests make no network calls and need no
  keys. CI never holds real API keys.
- Integration tests run a recorded proposal through Preview -> Accept -> Undo/Redo and check
  that a rejected proposal leaves the document unchanged.
- Musical quality cannot be proven by tests; listening checks are recorded as manual checks.

## Before release

- Check the Anthropic and OpenAI API terms for client applications, and say in the app that
  requests go to the chosen provider under its terms.
- Confirm no key, prompt or response is written to logs, crash reports or project files.
- Keep all bundled prompts, templates and generator rules original.

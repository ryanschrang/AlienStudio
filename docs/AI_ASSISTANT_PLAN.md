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

## Splice connection (optional, can ship before the assistant)

Splice runs a hosted MCP server (beta at the time of writing) at `https://mcp.splice.com/mcp`,
using Streamable HTTP and OAuth sign-in. It offers sample search (`describe_a_sound`), stacks
(`create_stack`, `prompt_to_stack`, `share_stack`) and downloads (`download_asset`).
Searching and building stacks is free. Downloads need a paid Splice subscription and use
credits; Splice's own limits apply.

**What it does not cover:** Splice's playable instruments are not offered through MCP. If
Splice publishes a VST3 version of its instrument plugin, it loads through the existing plugin
hosting (T1-10), separately from this connection.

Two ways to use it:

1. **Splice tab in the Sample Browser, with no AI needed.** Alien Studio acts as a small MCP
   client. The user signs in once in their web browser (OAuth with a loopback redirect); the
   login token is stored with Windows Credential Manager, never in projects or logs. Search
   results appear in the browser, and Download saves the file to a local Splice folder, then
   uses the existing preview, tempo/key matching, drag/drop and import paths (BROWSE-01).
2. **Through the assistant.** The Claude and OpenAI APIs can both call remote MCP servers, so
   the assistant can search Splice for a sound that fits the song ("a dusty 90 BPM snare in
   this key"), then offer it for preview and import.

Rules:

- **Downloads spend the user's credits, so they are always a deliberate user click** that
  shows the credit cost. The assistant may search and suggest, but never downloads without
  confirmation.
- Shared stack links are public; the app says so before sharing.
- Disconnect removes the stored login. Splice is never required for any feature.
- Before building, check Splice's terms for a DAW connecting directly (it may need a partner
  agreement) and whether the server is still beta.

Increments:

- **S1: MCP client.** JSON-RPC over Streamable HTTP, tool listing and calls, timeouts,
  cancellation, background thread.
- **S2: Splice sign-in.** OAuth in the system browser, Credential Manager storage, sign-out.
- **S3: Splice tab in the Sample Browser.** Search, results, preview where offered.
- **S4: Download and import.** Credit-cost confirmation, local Splice folder, then the existing
  import path with one Undo step.
- **S5: Assistant access (after J5).** The assistant's provider request includes the Splice
  server, so it can search and suggest; downloads still need the user's click.

Tests use recorded MCP responses (fixtures), so CI needs no Splice account. A real sign-in and
download are manual checks.

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

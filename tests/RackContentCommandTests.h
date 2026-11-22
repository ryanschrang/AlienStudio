#pragma once
static void rackNotesTransactionChecks() {
    auto engine = std::make_unique<ToneEngine>();
    auto controller = std::make_unique<alien::ProjectController>(*engine);
    auto& c = *controller;
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto& d = c.document();
    const auto source = d.channelRack.clips[0];
    require(commands.execute(cmd::SetRackStep{0, 1, 4, true}).changed
        && commands.execute(cmd::AssignRackClip{0, 0, source}).changed
        && commands.execute(cmd::PlaceSongClip{alien::timeline::rackRowBase, 0, 0, 2 * alien::timeline::ticksPerBar}).changed,
        "a rack source shared by Session and Song with dormant steps");
    const auto region = d.timeline.regions.front().id;
    c.markAsNewBaseline();
    const auto before = d;
    const auto history = historyOf(c);
    require(engine->rackMailbox.consume() != nullptr, "consume the fixture's engine bank");
    auto notes = std::make_shared<alien::PianoRollPattern>();
    notes->loopSteps = 32;
    notes->add(48, 20, 8, .4f); notes->add(60, 20, 4, .9f); notes->add(67, 20, 6, .6f);
    const std::vector<cmd::Command> edits{cmd::SetRackPatternLength{0, 32}, cmd::SetRackChannelNotes{0, 1, notes}};
    const auto prediction = commands.preview(edits);
    require(prediction.ok && prediction.changed && alien::sameContent(before, d) && historyOf(c) == history
        && engine->rackMailbox.consume() == nullptr, "length-plus-notes Preview leaves the live source and engine untouched");
    require(commands.batch(edits, "Extend pattern and write notes").changed && alien::sameContent(prediction.after, d)
        && d.channelRack.pattern(0).length == 32 && d.channelRack.pattern(0).steps[1][4].on
        && d.channelRack.clips[0] == source && d.clip(0, 0).rackClip == source
        && d.timeline.find(region)->rackClip == source && d.rackMatchesLibrary(),
        "one batch updates the shared source and keeps dormant steps and all bindings");
    const auto* published = engine->rackMailbox.consume();
    const auto* publishedPattern = published ? published->pattern(0) : nullptr;
    require(publishedPattern && published->clips[0] == source && publishedPattern->notes[1]
        && alien::rack::Pattern::sameNotes(*publishedPattern->notes[1], *notes),
        "engine receives the full overlapping chord, lengths and velocities");
    require(c.undo() && alien::sameContent(before, d) && !d.channelRack.isNoteChannel(0, 1),
        "one Undo restores length and step mode together");
    const auto* restored = engine->rackMailbox.consume();
    const auto* restoredPattern = restored ? restored->pattern(0) : nullptr;
    require(restoredPattern && restoredPattern->length == 16 && !restoredPattern->notes[1], "Undo restores the engine's step source");
    const auto redo = historyOf(c);
    const auto unchanged = commands.execute(cmd::SetRackChannelNotes{0, 1,
        std::make_shared<const alien::PianoRollPattern>(alien::rack::channelNotes(d.channelRack.pattern(0), 1))});
    require(unchanged.ok && !unchanged.changed && historyOf(c) == redo && engine->rackMailbox.consume() == nullptr,
        "opening unchanged step notes preserves redo without engine publication");
    const auto refused = commands.batch({cmd::SetRackPatternLength{0, 32}, cmd::SetRackChannelNotes{0, 99, notes}}, "Refused notes");
    require(!refused.ok && refused.failedCommand == 1 && alien::sameContent(before, d)
        && historyOf(c) == redo && engine->rackMailbox.consume() == nullptr,
        "a late note refusal keeps the old length, source, engine and redo");
    c.setExternalBusy(true);
    const auto busy = commands.execute(cmd::SetRackChannelNotes{0, 1, notes});
    c.setExternalBusy(false);
    require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, d) && historyOf(c) == redo
        && engine->rackMailbox.consume() == nullptr, "busy note edits refuse before document or engine changes");
    require(c.redo() && alien::sameContent(prediction.after, d)
        && alien::rack::Pattern::sameNotes(alien::rack::channelNotes(d.channelRack.pattern(0), 1), *notes),
        "Redo still restores the complete accepted length-plus-notes batch");
}

// Channel notes, drum grooves and sample loading. ChannelRackViewTests drives the real pattern menu, drops and async decode.
static void rackContentCommandChecks() {
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    c.markAsNewBaseline();
    const auto& rack = c.document().channelRack;
    require(commands.execute(cmd::AddRackChannel{"kick"}).changed, "content command fixture");
    const int channel = rack.channelCount - 1, pattern = rack.selectedPattern;
    const auto notesOf = [&](int ch) { return alien::rack::channelNotes(rack.pattern(pattern), ch); };
    const auto shared = [](const alien::PianoRollPattern& notes) { return std::make_shared<const alien::PianoRollPattern>(notes); };

    // Notes: Preview touches nothing, one key is one Undo step, and the notes a step channel already shows change nothing.
    {
        const auto history = historyOf(c);
        require(commands.execute(cmd::SetRackChannelNotes{pattern, channel, shared(notesOf(channel))}).ok && historyOf(c) == history && !rack.isNoteChannel(pattern, channel),
                "opening a step channel's own notes does not make it a note channel");
        auto edited = notesOf(channel);
        edited.add(60, 2.0, 1.0);
        const auto before = c.document();
        const auto preview = commands.preview(cmd::SetRackChannelNotes{pattern, channel, shared(edited)});
        require(preview.ok && preview.changed && alien::sameContent(before, c.document()) && historyOf(c) == history && !rack.isNoteChannel(pattern, channel), "SetRackChannelNotes Preview changes no live state");
        require(commands.execute(cmd::SetRackChannelNotes{pattern, channel, shared(edited)}, "roll-1").changed && alien::sameContent(preview.after, c.document())
            && rack.isNoteChannel(pattern, channel) && alien::rack::Pattern::sameNotes(notesOf(channel), edited) && c.undoLabel() == "Edit channel notes", "SetRackChannelNotes does what Preview said");
        edited.add(62, 3.0, 1.0);
        require(commands.execute(cmd::SetRackChannelNotes{pattern, channel, shared(edited)}, "roll-1").changed, "a second change in the gesture");
        require(c.undo() && !rack.isNoteChannel(pattern, channel), "one gesture is one Undo step");
        require(c.redo() && alien::rack::Pattern::sameNotes(notesOf(channel), edited), "and Redo brings all of it back");
        require(c.undo(), "notes fixture undoes");
    }

    // A groove: a new selected pattern, Preview agrees and allocates nothing live, one Undo step.
    {
        alien::midi_import::ImportedClip clip;
        clip.lengthTicks = alien::PianoRollPattern::ticksPerStep * 16;
        alien::midi_import::ImportedNote kick;
        kick.channel = alien::midi_import::gmDrumChannel;
        kick.pitch = 36;
        kick.startTicks = 0;
        kick.lengthTicks = 60;
        kick.velocity = 0.8f;
        clip.notes.push_back(kick);
        require(clip.ok(), "groove fixture is a usable clip");
        const auto groove = std::make_shared<const alien::midi_import::ImportedClip>(clip);
        const auto before = c.document();
        const int patterns = rack.patternCount;
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::ImportRackGroove{groove, "Beat"});
        require(preview.ok && preview.changed && rack.patternCount == patterns && historyOf(c) == history && alien::sameContent(before, c.document()), "ImportRackGroove Preview changes no live state");
        require(commands.execute(cmd::ImportRackGroove{groove, "Beat"}).changed && alien::sameContent(preview.after, c.document()) && rack.patternCount == patterns + 1
            && rack.selectedPattern == patterns && rack.patternNames[static_cast<std::size_t>(patterns)] == "Beat" && c.undoLabel() == "Import drum groove", "ImportRackGroove does what Preview said");
        require(c.undo() && rack.patternCount == patterns && c.redo() && rack.patternCount == patterns + 1, "and undoes and redoes");
        require(c.undo() && rack.selectedPattern == pattern, "groove fixture undoes");

        alien::midi_import::ImportedClip melodic = clip;
        melodic.notes[0].channel = 1;
        const auto says = commands.execute(cmd::ImportRackGroove{std::make_shared<const alien::midi_import::ImportedClip>(melodic), "Tune"});
        require(!says.ok && says.error.contains("no drum notes") && says.error.contains("Tune"), "a file with no drums is refused by name");
        const auto none = commands.execute(cmd::ImportRackGroove{nullptr, "Nothing"});
        require(!none.ok && none.error.contains("no drum groove"), "a missing groove is refused");
    }

    // A sample: Preview holds no audio, the channel takes the file's name, and Undo leaves the audio for Redo.
    {
        const auto decoded = [](int frames) {
            auto clip = std::make_shared<alien::SessionAudioClip>();
            clip->sampleRate = 8000; clip->channels = 1; clip->frames = frames;
            clip->left.assign(static_cast<std::size_t>(frames), 0.25f);
            clip->buildWaveform();
            return clip;
        };
        const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rack content boom.wav");
        const auto path = file.getFullPathName();
        const auto available = c.availableAudioBytes();
        const auto first = decoded(800);
        const cmd::LoadRackSample load{channel, path, "rack content boom", first};
        const auto preview = commands.preview(load);
        require(preview.ok && preview.changed && c.availableAudioBytes() == available && rack.info[static_cast<std::size_t>(channel)].sample == "builtin:kick", "LoadRackSample Preview holds no audio and changes no live state");

        std::vector<juce::String> statuses;
        c.onImportStatus = [&](const juce::String& text, const juce::String&) { statuses.push_back(text); };
        const auto result = cmd::loadDecodedRackSample(c, file, channel, first);
        require(result.changed && alien::sameContent(preview.after, c.document()) && rack.info[static_cast<std::size_t>(channel)].sample == path.toStdString()
            && rack.info[static_cast<std::size_t>(channel)].name == "rack content boom" && !c.rackChannelOffline(channel) && c.undoLabel() == "Load channel sample", "LoadRackSample does what Preview said");
        require(c.availableAudioBytes() == available - first->decodedBytes() && !statuses.empty() && statuses.back().contains("Loaded"), "it holds the audio and says so");
        require(c.undo() && rack.info[static_cast<std::size_t>(channel)].sample == "builtin:kick" && c.redo() && !c.rackChannelOffline(channel), "Undo and Redo keep the audio playable");

        // The same file again is no edit and no Undo step, but its new decode is what the engine holds.
        const auto history = historyOf(c);
        const auto again = decoded(400);
        const auto repeat = cmd::loadDecodedRackSample(c, file, channel, again);
        require(repeat.ok && !repeat.changed && historyOf(c) == history && c.availableAudioBytes() == available - first->decodedBytes() - again->decodedBytes(), "the same file again keeps its new decode with no Undo step");

        // Refusals hold no audio.
        const auto held = c.availableAudioBytes();
        const auto missing = cmd::loadDecodedRackSample(c, file, 99, decoded(100));
        require(!missing.ok && missing.error.contains("no rack channel") && c.availableAudioBytes() == held && statuses.back() == missing.error, "a missing channel is refused and reported, holding no audio");
        const auto noAudio = commands.execute(cmd::LoadRackSample{channel, path, "x", nullptr});
        require(!noAudio.ok && noAudio.error.contains("no decoded audio") && c.availableAudioBytes() == held, "no audio is refused");
        const auto noPath = commands.execute(cmd::LoadRackSample{channel, "", "x", decoded(100)});
        require(!noPath.ok && noPath.error.contains("needs a file"), "no path is refused");
        const auto longPath = commands.execute(cmd::LoadRackSample{channel, juce::String::repeatedString("a", alien::rack::maxSampleBytes + 1), "x", decoded(100)});
        require(!longPath.ok && longPath.error.contains("too long"), "a path too long to store is refused");
        c.onImportStatus = nullptr;
        require(c.undo(), "sample fixture undoes");
    }

    // Refusals change nothing, and a busy project refuses all three.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto notes = notesOf(channel);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::SetRackChannelNotes{pattern, channel, nullptr}, "cannot be stored");
        says(cmd::SetRackChannelNotes{99, channel, shared(notes)}, "no rack pattern");
        says(cmd::SetRackChannelNotes{pattern, 99, shared(notes)}, "no rack channel");
        auto wrongLength = notes;
        wrongLength.loopSteps = rack.pattern(pattern).length * 2;
        says(cmd::SetRackChannelNotes{pattern, channel, shared(wrongLength)}, "another length");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        auto edited = notes;
        edited.add(64, 1.0, 1.0);
        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::SetRackChannelNotes{pattern, channel, shared(edited)});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && historyOf(c) == history, "a busy project refuses them");
    }

    // None has a script form.
    juce::String error;
    for (const auto* commandName : {"SetRackChannelNotes", "ImportRackGroove", "LoadRackSample"}) {
        error.clear();
        require(!cmd::parse(commandName, juce::JSON::parse("{\"pattern\":0,\"channel\":0}"), error) && error.contains("cannot be written"), "content commands have no script form");
    }
}

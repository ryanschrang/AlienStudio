#pragma once
// PlaceSongClip: putting a Session cell or a rack pattern in the Song. TimelineViewTests drives the real double-click
// and drop placements, and ProjectControllerTests the controller's placeRegion, which shares this reducer.
static void songPlaceCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    require(c.document().trackCount() >= 3 && c.document().sceneCount() >= 2 && c.channelRack().patternCount >= 1, "place fixture size");
    auto audio = std::make_shared<alien::SessionAudioClip>();
    audio->sampleRate = 8000; audio->channels = 1; audio->frames = 32000;   // 4 s: two bars at 120 BPM
    audio->left.assign(32000, .2f); audio->buildWaveform();
    require(c.installAudio(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("place-command-fixture.wav"), 2, 0, audio), "audio place fixture");
    const auto source = c.channelRack().clips[0];
    require(c.assignRackClip(1, 1, source), "a Session cell holding a rack clip");
    c.markAsNewBaseline();
    const auto found = [&](std::uint32_t id) { return song.find(id); };

    // Every placement: Preview changes nothing live and hands out the id the live edit takes, execute does what Preview
    // said under the view's Undo name, and Undo and Redo work. Returns the placed clip's id.
    const auto step = [&](const cmd::PlaceSongClip& place, const char* label, const std::function<bool(const tl::Region&)>& happened, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(cmd::Command{place});
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), what);
        const auto predicted = alien::song_clip::placedClip(before, before.timeline, preview.after.timeline, place.row, place.source, place.start);
        require(alien::song_clip::placesPattern(before, place.row, place.source) == (juce::String(label) == "Place pattern"), what);
        const auto result = commands.batch({place}, label);
        const auto id = alien::song_clip::placedClip(before, before.timeline, song, place.row, place.source, place.start);
        require(result.ok && result.changed && id != 0 && id == predicted && alien::sameContent(preview.after, c.document()) && c.undoLabel() == label
                && happened(*found(id)), what);
        require(c.undo() && alien::sameContent(before, c.document()) && found(id) == nullptr, what);
        require(c.redo() && found(id) != nullptr && happened(*found(id)), what);
        return id;
    };
    const auto midiLength = c.naturalLength(0, 0);
    require(midiLength > 0 && midiLength == alien::song_clip::naturalLength(c.document(), 0, 0), "the controller's natural length is the reducer's");
    const auto midi = step({0, 0, bar}, "Place clip", [&](const tl::Region& r) { return r.row == 0 && r.source == 0 && r.start == bar && r.length == midiLength && !r.isPattern(); },
                           "PlaceSongClip puts a MIDI cell in the song at its loop length");
    step({2, 0, 0}, "Place clip", [&](const tl::Region& r) { return r.row == 2 && r.length == 2 * bar; }, "an audio cell lasts as long as its audio at the song's tempo");
    step({0, 1, 6 * bar, 3 * tl::ticksPerStep}, "Place clip", [&](const tl::Region& r) { return r.start == 6 * bar && r.length == 3 * tl::ticksPerStep; },
         "a length given is the length placed");
    step({tl::rackRowBase + 1, 0, 0}, "Place pattern", [&](const tl::Region& r) { return r.row == tl::rackRowBase + 1 && r.isPattern() && r.rackClip == source; },
         "a rack pattern goes on its lane");
    step({1, 1, 2 * bar}, "Place pattern", [&](const tl::Region& r) { return r.row == tl::rackRowBase && r.isPattern() && r.rackClip == source && r.start == 2 * bar; },
         "a Session cell holding a rack clip places its pattern on the first rack lane");
    // A placement covers what it lands on: the earlier MIDI clip on track 1 is replaced.
    const auto count = song.regions.size();
    step({0, 1, bar}, "Place clip", [&](const tl::Region& r) { return r.source == 1 && found(midi) == nullptr; }, "a placement replaces the clip it covers");
    require(song.regions.size() == count, "replacing a clip leaves as many clips");
    const auto longSong = c.document().timeline.lengthBars;
    step({0, 0, static_cast<std::int64_t>(longSong + 4) * bar}, "Place clip", [&](const tl::Region&) { return song.lengthBars > longSong; }, "a placement past the end makes the song longer");

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::PlaceSongClip& place, const char* why) {
            const auto preview = commands.preview(cmd::Command{place});
            const auto result = commands.execute(place);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says({99, 0, 0}, "no track 100");
        says({-1, 0, 0}, "no track 0");
        says({0, 99, 0}, "no scene 100");
        says({0, -1, 0}, "no scene 0");
        says({tl::rackRowBase, 99, 0}, "no rack pattern 100");
        says({0, 0, -1}, "inside the song");
        says({0, 0, tl::maxLength}, "inside the song");
        says({0, 0, 0, -1}, "length must be");
        says({0, 0, 0, tl::maxLength + 1}, "length must be");
        says({0, 0, tl::maxLength - 1, 2 * bar}, "does not fit");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::PlaceSongClip{0, 0, 0});
        const bool controllerBusy = c.placeRegion(0, 0, 0);
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && !controllerBusy && alien::sameContent(before, c.document()), "a busy project refuses a placement");

        auto frozen = c.document();
        frozen.tracks[0].settings.freezeScene = 0;
        require(cmd::applyTo(frozen, cmd::PlaceSongClip{0, 0, 0}).contains("Unfreeze track 1"), "a frozen track takes no new clip");
        require(cmd::applyTo(frozen, cmd::PlaceSongClip{1, 0, 0}).isEmpty(), "another track still does");
    }

    // The controller's placeRegion is the same edit, under the same Undo names, and says which clip it placed.
    {
        const auto before = c.document();
        std::uint32_t id = 0;
        require(c.placeRegion(2, 1, 12 * bar, &id) && c.undoLabel() == "Place clip" && found(id) != nullptr && found(id)->start == 12 * bar, "placeRegion places a clip");
        require(c.undo() && alien::sameContent(before, c.document()), "placeRegion is one Undo step");
        require(c.placeRegion(1, 1, 12 * bar, &id) && c.undoLabel() == "Place pattern" && found(id)->row == tl::rackRowBase, "placeRegion places a cell's rack pattern");
        require(c.undo(), "undo the pattern");
    }

    // Script form.
    juce::String error;
    for (const auto& command : std::vector<cmd::Command>{cmd::PlaceSongClip{3, 2, 960}, cmd::PlaceSongClip{tl::rackRowBase + 2, 1, 0, 480}}) {
        error.clear();
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
                "PlaceSongClip round-trips through JSON");
    }
    const auto parsed = cmd::parse("PlaceSongClip", juce::JSON::parse(R"({"row": 0, "source": 1, "start": 1920})"), error);
    require(parsed && std::get<cmd::PlaceSongClip>(*parsed).length == 0 && std::get<cmd::PlaceSongClip>(*parsed).start == 1920, "the length may be left out");
    require(!cmd::arguments(cmd::Command{cmd::PlaceSongClip{0, 1, 0}}).getDynamicObject()->hasProperty("length"), "a natural length is not written");
    error.clear();
    require(!cmd::parse("PlaceSongClip", juce::JSON::parse(R"({"row": 0, "start": 0})"), error) && error.contains("source"), "PlaceSongClip needs a source");
    error.clear();
    require(!cmd::parse("PlaceSongClip", juce::JSON::parse(R"({"row": 0, "source": 0, "start": 0, "clip": 3})"), error) && error.contains("clip"), "PlaceSongClip has no other arguments");
    error.clear();
    require(!cmd::parse("PlaceSongClip", juce::JSON::parse(R"({"row": 0, "source": 0, "start": 0, "length": 1.5})"), error) && error.contains("length"), "a length is whole");
    require(cmd::describe(c.document(), cmd::PlaceSongClip{0, 1, 960}).contains("scene 2 of track 1"), "PlaceSongClip describes a cell");
    require(cmd::describe(c.document(), cmd::PlaceSongClip{tl::rackRowBase + 1, 0, 0}).contains("rack pattern 1 on rack lane 2"), "PlaceSongClip describes a pattern");
    require(cmd::describe(c.document(), cmd::PlaceSongClip{1, 1, 0}).contains("rack lane 1"), "a rack cell is described as the pattern it places");
}

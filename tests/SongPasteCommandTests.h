#pragma once
// PasteSongClips: pasting copied Song clips, which brings back a placement clip that was cut (or edited since it was
// copied) as part of its own Undo step. TimelineViewTests drives the real Ctrl+V and Paste menu, which run this command.
static void songPasteCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    const auto& d = c.document();
    c.setClipPattern(0, 0, patternWith(60), false); c.commit("Notes", 0, 0);
    std::uint32_t a = 0, other = 0;
    // Only a shared placement can be made unique, so the cell is placed twice.
    require(c.placeRegion(0, 0, 0, &a) && c.placeRegion(0, 0, 8 * bar, &other) && c.makePlacementUnique(a), "a placement clip to copy");
    const auto clipOf = [&](std::uint32_t id) { return song.find(id)->clip; };
    const auto original = clipOf(a);
    require(original != 0 && d.library.clip(alien::library::ClipId{original}) != nullptr, "the region plays a clip of its own");
    c.markAsNewBaseline();

    // What the Song view's Copy remembers: the regions, and the placement clips' notes and names as they were.
    const auto copyOf = [&](const tl::Ids& ids) {
        auto pasted = std::make_shared<alien::song_clip::Pasted>();
        pasted->clips = song.copy(ids);
        for (const auto& r : pasted->clips.regions)
            if (r.ownsClip()) pasted->placements[r.clip] = {d.contentOf(alien::library::ClipId{r.clip}).pattern, d.placementName(r.clip)};
        return pasted;
    };
    // Every paste: Preview changes nothing live and names the clips the live edit pastes, execute does what Preview said
    // in one Undo step named "Paste", and Undo and Redo work. Returns the pasted clips' ids.
    const auto step = [&](const cmd::PasteSongClips& paste, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto predicted = alien::song_clip::pastedClips(before, *cmd::songClipEdit(paste));
        const auto preview = commands.preview(cmd::Command{paste});
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()) && !predicted.empty(), what);
        const auto result = commands.batch({paste}, cmd::label(paste));
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == "Paste", what);
        for (const auto id : predicted) require(song.find(id) != nullptr && before.timeline.find(id) == nullptr, what);
        const auto after = c.document();
        require(c.undo() && alien::sameContent(before, c.document()) && d.library.clipCount() == before.library.clipCount(), what);
        require(c.redo() && alien::sameContent(after, c.document()), what);
        return predicted;
    };

    // While the copied clip still holds what was copied, a paste shares it.
    const auto copied = copyOf({a});
    const auto clips = d.library.clipCount();
    const auto shared = step({copied, 4 * bar, 0}, "PasteSongClips pastes at the tick it is given");
    require(shared.size() == 1 && song.find(shared[0])->start == 4 * bar && clipOf(shared[0]) == original && d.library.clipCount() == clips,
            "a paste of a clip that is still there shares it");

    // Cut: deleting every region that plays it drops the clip. The paste brings it back with the copied notes, in the
    // same Undo step, so Undo takes the brought-back clip away too.
    require(commands.execute(cmd::DeleteSongClips{{a, shared[0]}}).changed && d.library.clip(alien::library::ClipId{original}) == nullptr, "the cut drops the clip");
    const auto cutClips = d.library.clipCount();
    const auto back = step({copied, 2 * bar, 0}, "a paste after a cut brings the clip back");
    const auto restored = clipOf(back[0]);
    require(restored != 0 && restored != original && d.library.clipCount() == cutClips + 1
            && d.placementContent(*song.find(back[0])).notes().notes[0].pitch == 60, "the brought-back clip has the copied notes");
    // Two copies of one cut clip bring back one clip that both play.
    {
        auto twice = std::make_shared<alien::song_clip::Pasted>(*copied);
        auto second = twice->clips.regions[0];
        second.start += second.length;   // right after the first, so they do not overlap
        twice->clips.regions.push_back(second);
        const auto both = step({twice, 16 * bar, 0}, "a paste of two regions of a cut clip");
        require(both.size() == 2 && clipOf(both[0]) == clipOf(both[1]) && clipOf(both[0]) != restored && d.library.clipCount() == cutClips + 2,
                "they share the clip brought back for them");
        require(c.undo(), "undo the double paste");
    }

    // Edited since it was copied: the paste brings back the copied notes as a clip of its own, and leaves the edit.
    const auto edited = copyOf({back[0]});
    require(c.setPlacementPattern(restored, patternWith(70)), "edit the clip"); c.commit("Edit notes");
    const auto fresh = step({edited, 24 * bar, 0}, "a paste after an edit");
    require(clipOf(fresh[0]) != restored && d.placementContent(*song.find(fresh[0])).notes().notes[0].pitch == 60
            && d.placementContent(*song.find(back[0])).notes().notes[0].pitch == 70, "the paste plays the copied notes and the edited clip keeps its edit");

    // A paste over the middle of a longer clip cuts it in two; the cut-off part is new too, but only the paste is named.
    {
        std::uint32_t wide = 0;
        require(c.placeRegion(1, 0, 32 * bar, &wide) && commands.execute(cmd::TrimSongClipEnd{wide, 40 * bar}).changed, "a long clip");
        auto small = std::make_shared<alien::song_clip::Pasted>();
        small->clips = song.copy({wide});
        small->clips.regions[0].length = bar;
        const auto regions = song.regions.size();
        const auto made = step({small, 34 * bar, 0}, "a paste inside a clip");
        require(made.size() == 1 && song.regions.size() == regions + 2 && song.find(made[0])->start == 34 * bar && song.find(made[0])->length == bar,
                "the paste is named, and the clip it landed in is in two");
    }

    // Rack patterns move by `lanes`.
    std::uint32_t pattern = 0;
    require(c.placeRegion(tl::rackRowBase, 0, 0, &pattern), "a rack pattern to copy");
    const auto patterns = copyOf({pattern});
    const auto lane = step({patterns, 8 * bar, 2}, "a pattern paste");
    require(song.find(lane[0])->row == tl::rackRowBase + 2, "a pattern pastes `lanes` lanes down");

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::PasteSongClips& paste, const char* why) {
            const auto preview = commands.preview(cmd::Command{paste});
            const auto result = commands.execute(paste);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
            require(alien::song_clip::pastedClips(c.document(), *cmd::songClipEdit(paste)).empty(), why);
        };
        says({nullptr, 0, 0}, "Nothing to paste");
        says({std::make_shared<alien::song_clip::Pasted>(), 0, 0}, "Nothing to paste");
        says({copied, -1, 0}, "inside the song");
        says({copied, tl::maxLength, 0}, "inside the song");
        says({copied, tl::maxLength - 1, 0}, "do not fit");
        says({patterns, 0, -1}, "do not fit");
        auto lost = std::make_shared<alien::song_clip::Pasted>(*copied);
        lost->placements.clear();
        says({lost, 0, 0}, "no longer in the project");
        auto bad = std::make_shared<alien::song_clip::Pasted>(*copied);
        bad->placements.begin()->second.first = nullptr;
        says({bad, 0, 0}, "no longer in the project");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::PasteSongClips{copied, 0, 0});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && alien::sameContent(before, c.document()), "a busy project refuses a paste");

        auto frozen = c.document();
        frozen.tracks[0].settings.freezeScene = 0;
        require(cmd::applyTo(frozen, cmd::PasteSongClips{copied, 48 * bar, 0}).contains("Unfreeze track 1"), "a frozen track takes no pasted clip");
        require(cmd::applyTo(frozen, cmd::PasteSongClips{patterns, 48 * bar, 0}).isEmpty(), "a rack lane still does");
    }

    // No script form: the copied clips exist only as data.
    juce::String error;
    require(!cmd::parse("PasteSongClips", juce::JSON::parse(R"({"at": 0, "lanes": 0})"), error) && error.contains("cannot be written"), "copied Song clips have no script form");
    require(cmd::describe(c.document(), cmd::PasteSongClips{copied, 960, 0}) == "Paste 1 copied Song clip at tick 960", "PasteSongClips describes itself");
    require(cmd::describe(c.document(), cmd::PasteSongClips{patterns, 0, 1}).contains("rack patterns 1 lane down"), "and says how far rack patterns move");
    require(cmd::label(cmd::Command{cmd::PasteSongClips{copied, 0, 0}}) == "Paste", "its Undo name is the Song view's");
}

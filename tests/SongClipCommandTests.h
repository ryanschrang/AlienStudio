#pragma once
// Song clip edits (move, trim, slip, delete, mute, gain, fades, crossfades, groups, loop and reverse).
// TimelineViewTests drives the real drags, keys and menus that now run them.
static void songClipCommandChecks() {
    namespace tl = alien::timeline;
    auto engine = std::make_unique<ToneEngine>();
    alien::ProjectController c(*engine);
    cmd::CommandEngine commands(c);
    c.pushDocumentToEngine();
    const auto bar = tl::ticksPerBar;
    const auto& song = c.timeline();
    // Two touching clips on track 1 and one on track 2.
    require(c.editTimeline("Place clips", [](tl::Timeline& t) { return t.placeBars(0, 0, 0, 1) && t.placeBars(0, 0, 1, 1) && t.placeBars(1, 0, 0, 1); }), "place Song clip fixture");
    c.markAsNewBaseline();
    std::uint32_t a = 0, b = 0, x = 0;
    for (const auto& r : song.regions) {
        if (r.row == 0 && r.start == 0) a = r.id;
        else if (r.row == 0 && r.start == bar) b = r.id;
        else if (r.row == 1) x = r.id;
    }
    require(a != 0 && b != 0 && x != 0, "fixture clips found");
    const auto at = [&](std::uint32_t id) { return song.find(id); };

    // Every edit: Preview changes nothing live, execute does what Preview said, one named Undo step, and Undo restores it.
    const auto step = [&](const cmd::Command& edit, const char* label, const std::function<bool()>& happened, const char* what) {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto preview = commands.preview(edit);
        require(preview.ok && preview.changed && historyOf(c) == history && alien::sameContent(before, c.document()), what);
        const auto result = commands.execute(edit);
        require(result.ok && result.changed && alien::sameContent(preview.after, c.document()) && c.undoLabel() == label && happened(), what);
        require(c.undo() && alien::sameContent(before, c.document()) && c.redo() && happened(), what);
    };
    const auto quarter = tl::ticksPerBeat;
    step(cmd::MoveSongClips{{x}, bar}, "Move clips", [&] { return at(x)->start == bar; }, "MoveSongClips");
    step(cmd::TrimSongClipEnd{x, bar + bar / 2}, "Resize clip", [&] { return at(x)->end() == bar + bar / 2; }, "TrimSongClipEnd");
    step(cmd::TrimSongClipStart{x, bar + quarter}, "Trim clip start", [&] { return at(x)->start == bar + quarter; }, "TrimSongClipStart");
    step(cmd::SlipSongClips{{x}, -quarter}, "Slip clips", [&] { return at(x)->offset == 0; }, "SlipSongClips");
    step(cmd::SetSongClipsMuted{{a, x}, true}, "Mute clips", [&] { return at(a)->muted && at(x)->muted; }, "SetSongClipsMuted");
    step(cmd::SetSongClipGain{{a}, 0.5}, "Set clip gain", [&] { return at(a)->gain == 0.5; }, "SetSongClipGain");
    step(cmd::SetSongClipFades{{a}, quarter, quarter}, "Fade clips", [&] { return at(a)->fadeIn == quarter && at(a)->fadeOut == quarter; }, "SetSongClipFades");
    step(cmd::SetSongClipFadeIn{a, 2 * quarter}, "Fade in", [&] { return at(a)->fadeIn == 2 * quarter; }, "SetSongClipFadeIn");
    step(cmd::SetSongClipFadeOut{a, 3 * quarter}, "Fade out", [&] { return at(a)->fadeOut == 3 * quarter; }, "SetSongClipFadeOut");
    step(cmd::SetSongClipFadeShape{{a}, tl::FadeEdge::in, tl::FadeCurve::sCurve}, "Fade shape", [&] { return at(a)->fadeInCurve == tl::FadeCurve::sCurve; }, "SetSongClipFadeShape");
    step(cmd::RemoveSongClipFades{{a}}, "Remove fades", [&] { return at(a)->fadeIn == 0 && at(a)->fadeOut == 0; }, "RemoveSongClipFades");
    step(cmd::CrossfadeSongClips{a, b, quarter}, "Crossfade clips", [&] { return at(b)->crossfade == quarter && at(a)->fadeOut == 0; }, "CrossfadeSongClips");
    const cmd::GroupSongClips group{{a, x}};
    step(group, "Group clips", [&] { return at(a)->group == group.group && at(x)->group == group.group; }, "GroupSongClips carries its group id");
    step(cmd::UngroupSongClips{{a}}, "Ungroup clips", [&] { return at(a)->group == 0 && at(x)->group == 0; }, "UngroupSongClips ungroups the whole group");
    step(cmd::LoopSongClips{{a}, true}, "Loop clip source", [&] { return at(a)->loopSource; }, "LoopSongClips");
    step(cmd::ReverseSongClips{{a}, true}, "Reverse clips", [&] { return at(a)->reversed; }, "ReverseSongClips");
    step(cmd::MoveSongClips{{x}, -bar}, "Move clips", [&] { return at(x)->start == quarter; }, "a move back");
    step(cmd::DeleteSongClips{{x}}, "Delete clips", [&] { return at(x) == nullptr; }, "DeleteSongClips");
    require(c.undo() && at(x) != nullptr, "Undo brings a deleted clip back under its id");

    // The engine plays the result when the command commits.
    {
        require(commands.execute(cmd::SetSongClipsMuted{{x}, false}).changed, "unmute for the engine check");
        engine->arrangementMailbox.consume();
        const auto* heard = engine->arrangementMailbox.current();
        const auto* playing = heard != nullptr ? heard->timeline.find(x) : nullptr;
        require(playing != nullptr && !playing->muted, "the engine plays the edited Song");
    }

    // Unchanged edits add no step and keep Redo; a caller can name the step and coalesce a run of them.
    {
        require(commands.execute(cmd::SetSongClipGain{{a}, 0.25}).changed && c.undo(), "Redo fixture");
        const auto redo = historyOf(c);
        for (const auto& same : std::vector<cmd::Command>{cmd::MoveSongClips{{a}, -bar}, cmd::SetSongClipGain{{a}, 0.5}, cmd::SetSongClipsMuted{{a}, true},
                                                         cmd::UngroupSongClips{{a}}, cmd::SlipSongClips{{x}, 0}}) {
            const auto r = commands.execute(same);
            require(r.ok && !r.changed && historyOf(c) == redo, "an unchanged Song clip edit adds no step and keeps Redo");
        }
        require(c.redo() && at(a)->gain == 0.25, "so Redo still works");
        require(commands.batch({cmd::SetSongClipGain{{a}, 1.0}, cmd::SetSongClipGain{{x}, 2.0}}, "Normalize clips").changed && c.undoLabel() == "Normalize clips"
                && at(a)->gain == 1.0 && at(x)->gain == 2.0, "several gains are one named step");
        require(c.undo() && at(a)->gain == 0.25 && at(x)->gain == 1.0, "and undo together");
    }

    // Refusals change nothing and say why.
    {
        const auto before = c.document();
        const auto history = historyOf(c);
        const auto says = [&](const cmd::Command& edit, const char* why) {
            const auto preview = commands.preview(edit);
            const auto result = commands.execute(edit);
            require(!preview.ok && !result.ok && result.error == preview.error && result.error.contains(why) && !result.changed, why);
        };
        says(cmd::DeleteSongClips{{}}, "at least one");
        says(cmd::MoveSongClips{{99999999}, bar}, "no clip");
        says(cmd::SetSongClipGain{{a}, 65.0}, "0 to 64");
        says(cmd::SetSongClipGain{{a}, -1.0}, "0 to 64");
        says(cmd::SetSongClipFades{{a}, -1, 0}, "negative");
        says(cmd::SetSongClipFadeIn{a, -1}, "negative");
        says(cmd::SetSongClipFadeShape{{a}, tl::FadeEdge::in, static_cast<tl::FadeCurve>(9)}, "fade shape");
        says(cmd::CrossfadeSongClips{a, x, quarter}, "touching");
        says(cmd::CrossfadeSongClips{b, a, quarter}, "touching");
        says(cmd::GroupSongClips{{a}}, "two or more");
        says(cmd::GroupSongClips{{a, x}, 0}, "already");
        require(alien::sameContent(before, c.document()) && historyOf(c) == history, "refusals change nothing");

        c.setExternalBusy(true);
        const auto busy = commands.execute(cmd::DeleteSongClips{{a}});
        c.setExternalBusy(false);
        require(!busy.ok && busy.error.contains("busy") && historyOf(c) == history, "a busy project refuses them");

        // A frozen track plays its render, so its clips are refused (the reducer, on a frozen copy of the document).
        auto frozen = c.document();
        frozen.tracks[1].settings.freezeScene = 0;
        require(alien::song_clip::refusal(frozen, *cmd::songClipEdit(cmd::MoveSongClips{{x}, bar})).contains("Unfreeze track 2"), "a frozen track's clips are refused");
        require(alien::song_clip::refusal(frozen, *cmd::songClipEdit(cmd::MoveSongClips{{a}, bar})).isEmpty(), "other tracks' clips are not");
    }

    // Script form: clip ids round-trip; a read GroupSongClips gets a fresh group id.
    juce::String error;
    for (const auto& command : std::vector<cmd::Command>{
             cmd::MoveSongClips{{3, 4}, -960, 1}, cmd::TrimSongClipStart{3, 480}, cmd::TrimSongClipEnd{3, 1920}, cmd::SlipSongClips{{3}, 240},
             cmd::DeleteSongClips{{3, 4}}, cmd::SetSongClipsMuted{{3}, true}, cmd::SetSongClipGain{{3}, 0.5}, cmd::SetSongClipFades{{3}, 120, 240},
             cmd::SetSongClipFadeIn{3, 60}, cmd::SetSongClipFadeOut{3, 90}, cmd::SetSongClipFadeShape{{3}, tl::FadeEdge::crossfade, tl::FadeCurve::fast},
             cmd::RemoveSongClipFades{{3}}, cmd::CrossfadeSongClips{3, 4, 480}, cmd::GroupSongClips{{3, 4}}, cmd::UngroupSongClips{{3}},
             cmd::LoopSongClips{{3}, true}, cmd::ReverseSongClips{{4}, false}}) {
        error.clear();
        const auto parsed = cmd::parse(cmd::name(command), juce::JSON::parse(juce::JSON::toString(cmd::arguments(command))), error);
        require(parsed.has_value() && parsed->index() == command.index() && juce::JSON::toString(cmd::arguments(*parsed)) == juce::JSON::toString(cmd::arguments(command)),
            "Song clip commands round-trip through JSON");
    }
    const auto first = cmd::parse("GroupSongClips", juce::JSON::parse(R"({"clips": [1, 2]})"), error);
    const auto second = cmd::parse("GroupSongClips", juce::JSON::parse(R"({"clips": [1, 2]})"), error);
    require(first && second && std::get<cmd::GroupSongClips>(*first).group != std::get<cmd::GroupSongClips>(*second).group, "each read GroupSongClips carries its own group id");
    for (const auto* bad : {R"({"clips": 3})", R"({"clips": [0]})", R"({"clips": [-2]})", R"({"clips": [4294967296]})", R"({"clips": ["3"]})"}) {
        error.clear();
        require(!cmd::parse("DeleteSongClips", juce::JSON::parse(bad), error) && error.contains("clip"), "clips must be a list of positive 32-bit ids");
    }
    error.clear();
    require(!cmd::parse("SetSongClipFadeShape", juce::JSON::parse(R"({"clips": [3], "edge": "middle", "curve": 0})"), error) && error.contains("edge"), "a fade edge is in, out or crossfade");
    error.clear();
    require(!cmd::parse("SetSongClipFadeShape", juce::JSON::parse(R"({"clips": [3], "edge": "in", "curve": 5})"), error) && error.contains("curve"), "a fade curve is in range");
    error.clear();
    require(!cmd::parse("CrossfadeSongClips", juce::JSON::parse(R"({"left": 3, "ticks": 0})"), error) && error.contains("right"), "a crossfade names both clips");
}

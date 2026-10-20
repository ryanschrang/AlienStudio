// Rebuild the dated PDF snapshot. No dependencies; Node 18+.
// Run from the repository root: node tools/build_feature_summary.mjs
import { writeFileSync } from "node:fs";
const pages = [
  {
    "title": "ALIEN STUDIO",
    "subtitle": "Feature summary & development milestones",
    "sections": [
      [
        "Snapshot | October 19, 2026",
        [
          "Windows 11 DAW prototype built with C++20, JUCE 8 and CMake.",
          "Baseline: main commit 8fbc9f483fe7. Recording and long offline tempo rendering are merged. This report also describes draft additions in the accompanying feature branch.",
          "Code present, automated coverage and physical-device verification are distinct statuses. This is not a claim of feature parity with established DAWs."
        ]
      ],
      [
        "Implemented foundations",
        [
          "Music creation: 12 built-in synth presets; polyphony; 16-step pitch/velocity sequencing; piano-roll notes, velocity/probability and processing; eight-pad sampled drums.",
          "MIDI input: external controllers, sustain and reconnection, plus typing/mouse piano. Recording live MIDI performances is still planned.",
          "Live sessions: up to 16 tracks and 32 scenes; beat/bar clip and scene launches; MIDI CC scene control. Audio clips are one-shots.",
          "Mixer: independent track instruments, volume, pan, mute/solo and meters.",
          "Projects: versioned save/open, portable audio collection, undo/redo, save prompts, autosave and recovery."
        ]
      ],
      [
        "Audio and plugin foundations",
        [
          "Recording: mono/stereo capture streams to disk with interrupted-take recovery. Independent multitrack arming and monitoring are not yet implemented.",
          "Tempo matching: offline pitch-preserving render to a separate WAV, with 0.5-2x duration ratios and bounded render buffers. Source files are preserved.",
          "Arrangement: bar-based regions and WAV export. Current model limits: 128 bars and 128 regions.",
          "VST3: initial stereo instruments, track/master FX and editors. User reports missing scanned plugins; compatibility and editor feedback remain incomplete."
        ]
      ]
    ]
  },
  {
    "title": "WHAT STILL NEEDS WORK",
    "subtitle": "Current limits and agreed delivery order",
    "sections": [
      [
        "Current limits to keep visible",
        [
          "Session audio is decoded into RAM: 128 MiB per clip and 512 MiB retained per project. Recording or rendering a long file does not make that file playable in Session.",
          "Tempo match is a render workflow, not source preview. Save the output WAV, wait for completion, then launch its loaded Session clip.",
          "User confirmed the render-and-launch workflow works for a supported file. This does not establish every MP3 codec, long source, plugin or audio device works.",
          "Plugin scanning is explicit per bundle. Some plugins do not appear; current generic errors need better diagnostics. Adding FX does not automatically select its editor slot."
        ]
      ],
      [
        "Agreed milestone order",
        [
          "1. Reliable long-file playback: decoder workers, per-playback cursors, prebuffering, waveforms and useful media errors.",
          "2. Audio editing: non-destructive trim, split, slip, duplicate, looping, fades and crossfades.",
          "3. Multitrack recording: arming, input routing, monitoring, punch recording, takes and latency compensation.",
          "4. MIDI recording: count-in, overdub, sustain capture, timing/quantization and controller editing.",
          "5. Mixer/plugin reliability: buses, sends, sidechains, multi-output plugins and latency compensation.",
          "6. Automation and timing: mixer/plugin automation, tempo/time-signature maps and editable warp markers.",
          "7. Delivery: freeze, stems, effect tails, dependable export and portable projects."
        ]
      ],
      [
        "Completion standard",
        [
          "Keep plugin and playback defects active throughout this sequence. For each milestone, require focused tests, Windows Debug/Release CI and a recorded manual scenario on the exact build.",
          "A feature checklist is maintained in docs/FEATURE_CHECKLIST.md. This PDF is a dated overview, not an automatically updating certification."
        ]
      ]
    ]
  },
  {
    "title": "REPRESENTATIVE PROJECTS",
    "subtitle": "Repeatable production, performance and recording checks",
    "sections": [
      [
        "Production | 120 BPM",
        [
          "examples/production/Production.alienstudio",
          "Four built-in synth tracks: bass, chords, lead and pulse. Four scenes form a 16-bar arrangement. No third-party media or plugins required.",
          "Acceptance: audible arrangement, Session audition, mute/solo/pan, note edits, undo/redo, save/reopen and comparison with exported WAV."
        ]
      ],
      [
        "Live performance | 128 BPM",
        [
          "examples/live/LivePerformance.alienstudio",
          "Four scenes: Build, Full, Break and Finish. Empty clips deliberately exercise transitions. Beat launches are the initial setting.",
          "Acceptance: switch beat/bar quantization, launch complete scenes, stop/relaunch, check stuck notes and device restart. Test physical MIDI CC separately."
        ]
      ],
      [
        "Recording reference | 120 BPM",
        [
          "examples/recording/MultitrackReference.alienstudio",
          "Four original mono test WAVs play simultaneously for four seconds. Empty Record Here, Alternate Take and Recovery scenes reserve destinations.",
          "Acceptance: solo distinct sources, record a new Audio lab take into an empty slot, render a tempo match, launch results, collect portable media and reopen.",
          "This is a reference session, not proof of simultaneous armed-track recording. That workflow remains planned."
        ]
      ],
      [
        "Long-file extension and results",
        [
          "tools/generate_reference_audio.py creates original reference WAVs in bounded blocks, from 1 to 3600 seconds. It refuses to replace existing files.",
          "Generate hours on demand instead of committing huge media. Baseline Session playback should still reject over-budget sources until streaming integration lands.",
          "Record build/device/sample rate/buffer size, expected and observed behavior, memory use and dropouts in examples/results/TEMPLATE.md. New fixture listening status: NOT RUN."
        ]
      ]
    ]
  },
  {
    "title": "FIRST STREAMING INCREMENT",
    "subtitle": "Foundation now; integration and hardware acceptance next",
    "sections": [
      [
        "Draft additions in this branch",
        [
          "StreamingAudioRing: fixed-capacity stereo SPSC storage, interpolation lookahead and explicit consumer advancement. Methods do not allocate, wait, lock or access files.",
          "Default sample storage is 128 KiB for 16,384 stereo frames, plus control/alignment storage. Each active playback instance needs independent state.",
          "Tests added for empty/full boundaries, partial writes, ring wrapping, stereo alignment and concurrent producer/consumer order.",
          "Three actual project fixtures and four original WAV sources, with a native-parser and media-resolution test.",
          "These additions do not yet remove Session playback limits. CI results must be checked on the PR; local builds and PDF visual rendering were unavailable in this disconnected session."
        ]
      ],
      [
        "Next integration work",
        [
          "Separate immutable clip metadata from per-playback decoders. Use bounded shared workers rather than a thread per loaded clip.",
          "Prebuffer quantized launches; define behavior if storage is late. Seek/relaunch must discard stale generations without resetting a ring under a consumer.",
          "Integrate large-file import, project reopen, recovered takes and tempo-render results. Preserve drum sample bounds and account for buffer/waveform memory.",
          "Build waveform peaks off-thread; expose missing media, decoder failures and underrun counts. Offline export must be deterministic and independent of worker timing."
        ]
      ],
      [
        "Acceptance before calling streaming complete",
        [
          "Test same-source concurrent playback, rapid stop/relaunch, missing/truncated media, slow decoding, sample-rate conversion, EOF and device changes.",
          "Play several hour-long sources with bounded memory and no callback I/O, allocation, locks or destruction. Use Session for hour-long checks; arrangement is currently capped at 128 bars.",
          "Run Windows Debug/Release tests and at least 15 minutes of audible multitrack playback. Verify save/reopen, portable media and live/export agreement within supported timeline lengths."
        ]
      ],
      [
        "Source and maintenance",
        [
          "Repository: github.com/ryanschrang/AlienStudio",
          "Primary evidence: README.md; src/SessionAudioClip.h; src/SessionProject.h; src/Arrangement.h; LongRecordingTests; LongTempoWarpTests; plugin hosting source.",
          "Maintained checklist: docs/FEATURE_CHECKLIST.md | Detailed plan: docs/LONG_PLAYBACK_PLAN.md | Scenarios: examples/README.md"
        ]
      ]
    ]
  }
];
function makePdf(pages){
 const objects=["<< /Type /Catalog /Pages 2 0 R >>","", "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>","<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>"];
 const kids=[];const esc=s=>s.replaceAll("\\","\\\\").replaceAll("(","\\(").replaceAll(")","\\)");
 const wrap=(s,max=89)=>{const out=[];let line="";for(const word of s.split(/\s+/)){if((line+" "+word).trim().length>max){out.push(line);line=word;}else line=(line+" "+word).trim();}if(line)out.push(line);return out;};
 pages.forEach((p,i)=>{let commands="";let y=790;const textAt=(s,x,y,size,font="F1",colour="0.15 0.20 0.26")=>{commands+=`${colour} rg BT /${font} ${size} Tf 1 0 0 1 ${x} ${y} Tm (${esc(s)}) Tj ET\n`;};
 commands+="0.10 0.33 0.46 rg 0 814 595 28 re f\n";
 textAt(p.title,44,y,19,"F2");y-=25;textAt(p.subtitle,44,y,11);y-=20;
 commands+=`0.60 0.73 0.78 RG 44 ${y} m 551 ${y} l S\n`;y-=25;
 for(const [heading,paras] of p.sections){textAt(heading,44,y,12,"F2","0.10 0.33 0.46");y-=19;for(const para of paras){for(const line of wrap(para)){textAt(line,44,y,10);y-=13;}y-=7;}y-=8;}
 if(y<48)throw Error(`page ${i+1} overflows: ${y}`);
 textAt(`Alien Studio | Feature summary | 2026-10-19`,44,28,8);textAt(`${i+1} / ${pages.length}`,516,28,8);
 const contentNumber=objects.length+2,pageNumber=objects.length+1;kids.push(`${pageNumber} 0 R`);
 objects.push(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents ${contentNumber} 0 R >>`);
 objects.push(`<< /Length ${commands.length} >>\nstream\n${commands}endstream`);
 });
 objects[1]=`<< /Type /Pages /Kids [${kids.join(" ")}] /Count ${pages.length} >>`;
 let pdf="%PDF-1.4\n";const offsets=[0];objects.forEach((o,i)=>{offsets.push(pdf.length);pdf+=`${i+1} 0 obj\n${o}\nendobj\n`;});
 const xref=pdf.length;pdf+=`xref\n0 ${objects.length+1}\n0000000000 65535 f \n`;for(const n of offsets.slice(1))pdf+=`${String(n).padStart(10,"0")} 00000 n \n`;pdf+=`trailer\n<< /Size ${objects.length+1} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
 if(/[^\x00-\x7f]/.test(pdf))throw Error("non-ASCII PDF");
 for(let i=1;i<offsets.length;i++)if(!pdf.slice(offsets[i]).startsWith(`${i} 0 obj\n`))throw Error("bad xref");
 return pdf;
}
writeFileSync("docs/Alien_Studio_Feature_Summary.pdf", makePdf(pages), "ascii");

# Alien Studio

Open-source Windows 11 DAW, early prototype.

## Build
Install Visual Studio 2022 with Desktop development with C++, Git, and CMake 3.24+.

```powershell
git clone https://github.com/ryanschrang/AlienStudio.git
cd AlienStudio
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The first version has a 16-step tone sequencer, play/stop and BPM control. CMake fetches JUCE when configuring. This has not yet been compiled or tested on Windows.

## Next
Piano roll, sampler, arrangement, recording, mixer, plugin hosting, and clip launching.

## License
License selection and JUCE dependency compatibility review pending before redistribution.

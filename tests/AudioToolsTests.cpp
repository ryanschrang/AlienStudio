#include "AudioCapture.h"
#include "TempoWarp.h"
#include "SamplePreviewRender.h"
#include "AudioFileWrite.h"
#include "ToneEngine.h"
#include <cstdlib>
#include <iostream>
static void check(bool ok,const char* message){if(!ok){std::cerr<<message<<'\n';std::exit(1);}}
int main(){
    alien::AudioCapture capture(8000,2,1);
    std::array<float,257> left{},right{};left.fill(0.25f);right.fill(-0.5f);const float* input[]{left.data(),right.data()};
    for(int i=0;i<35;++i)capture.append(input,2,257);
    check(capture.done.load() && capture.frames.load()==8000,"recording stops exactly at capacity");
    auto take=capture.finish();check(take && take->valid() && take->frames==8000,"bounded capture produces valid clip");
    check(take->left.front()==0.25f && take->right.back()==-0.5f,"stereo input remains separate");
    auto engine=std::make_unique<ToneEngine>();engine->prepare(48000);alien::AudioCapture deviceTake(48000,1,1);engine->capture.store(&deviceTake);
    std::array<float,257> output{};float* outputs[]{output.data()};
    engine->audioDeviceIOCallbackWithContext(input,2,outputs,1,257,{});
    check(deviceTake.frames.load()==257,"actual callback captures with transport stopped");
    for(auto sample:output)check(sample==0,"capture does not enable monitoring");
    engine->audioDeviceStopped();check(deviceTake.done.load(),"device stop seals take");engine->capture.store(nullptr);
    auto deviceClip=deviceTake.finish();check(deviceClip->left.back()==0.25f,"device capture survives stop");
    alien::AudioCapture missing(8000,1,1);missing.append(nullptr,0,17);check(missing.finish()->left[0]==0,"missing input gives defined silence");
    alien::SessionAudioClip source;source.sampleRate=48000;source.channels=2;source.frames=48000;
    source.left.resize(source.frames);source.right.resize(source.frames);
    for(std::size_t i=0;i<source.frames;++i){source.left[i]=static_cast<float>(0.5*std::sin(6.283185307179586*440*i/48000));source.right[i]=-source.left[i];}
    source.buildWaveform();const auto original=source.left;std::atomic<bool> cancel{false};
    for(double ratio:{0.5,0.8,1.0,1.5,2.0}){
        auto result=alien::warpTempo(source,ratio,cancel);
        check(result && result->valid() && result->frames==static_cast<std::size_t>(std::ceil(source.frames*ratio)),"warp has exact requested duration");
        int crosses=0;const auto begin=static_cast<std::size_t>(4800),end=result->frames-4800;
        for(std::size_t i=begin;i<end;++i){if(result->left[i-1]<=0 && result->left[i]>0)++crosses;
            check(std::abs(result->left[i]+result->right[i])<1e-6,"linked stereo warp preserves anti-phase relationship");}
        const double hz=crosses*48000.0/(end-begin);check(hz>435 && hz<445,"tempo change retains 440 Hz pitch");
        if(ratio==1)check(result->left==source.left,"unity warp is exact bypass");
    }
    check(source.left==original,"warp leaves source unchanged");
    check(!alien::warpTempo(source,0.49,cancel),"invalid ratio rejected");cancel.store(true);
    check(!alien::warpTempo(source,1.5,cancel),"cancelled warp produces no partial asset");
    auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("alien-take-test",".wav");juce::String error;cancel.store(false);
    check(alien::writeClipWav(file,*take,cancel,error),"take saves to WAV");
    juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(file.createInputStream().release(),true));
    check(reader && reader->lengthInSamples==8000 && reader->numChannels==2 && reader->sampleRate==8000,"recorded WAV metadata");
    juce::AudioBuffer<float> decoded(2,8000);check(reader->read(&decoded,0,8000,0,true,true),"recorded WAV readable");
    check(std::abs(decoded.getSample(1,7999)+0.5f)<1e-6,"recorded WAV contains captured input");reader.reset();
    juce::MemoryBlock before,after;file.loadFileAsData(before);cancel.store(true);
    check(!alien::writeClipWav(file,*take,cancel,error),"cancelled write rejected");file.loadFileAsData(after);
    check(before==after,"cancelled write preserves every byte");
    cancel.store(false); check(alien::writeClipWav(file, source, cancel, error), "write preview pitch fixture"); file.loadFileAsData(before);
    for (double bpm : {60., 120., 180., 240.}) {
        auto preview = alien::renderSamplePreview(file, true, bpm, 120, cancel);
        check(preview.clip && preview.clip->valid() && preview.clip->frames == static_cast<std::size_t>(48000 * bpm / 120), "synced preview duration follows source/project BPM ratio");
        int crosses = 0; const auto& data = preview.clip->left; const std::size_t begin = 4800, end = data.size() - 4800;
        for (std::size_t i = begin; i < end; ++i) { if (data[i-1] <= 0 && data[i] > 0) ++crosses; check(std::abs(data[i] + preview.clip->right[i]) < 1e-6, "preview stretch keeps stereo phase linked"); }
        const double hz = crosses * 48000.0 / (end - begin); check(hz > 435 && hz < 445, "preview keeps 440 Hz pitch when matching tempo");
    }
    auto plain = alien::renderSamplePreview(file, false, 0, 0, cancel);
    auto unity = alien::renderSamplePreview(file, true, 120, 120, cancel);
    check(plain.clip && unity.clip && plain.clip->left == unity.clip->left, "equal-tempo preview is exact decoded bypass");
    check(!alien::renderSamplePreview(file, true, 30, 200, cancel).clip, "unsupported tempo ratio refuses preview");
    check(!alien::validPreviewTempoRatio(std::numeric_limits<double>::quiet_NaN(), 120), "nonfinite preview BPM refused");
    check(alien::previewKeyShift(0, 2) == 2 && alien::previewKeyShift(0, 11) == -1 && alien::previewKeyShift(0, 6) == 6
          && alien::previewKeyShift(0, 7) == -5 && alien::previewKeyShift(9, 0) == 3, "key shift takes the nearer way round");
    struct KeyCase { bool sync; double bpm; int semitones; };
    for (const auto c : {KeyCase{false, 120, 3}, KeyCase{false, 120, -5}, KeyCase{true, 180, 2}, KeyCase{true, 90, -2}, KeyCase{false, 120, 6}}) {
        auto shifted = alien::renderSamplePreview(file, c.sync, c.bpm, 120, cancel, c.semitones);
        check(shifted.clip && shifted.clip->valid() && shifted.clip->frames == static_cast<std::size_t>(std::ceil(48000 * (c.sync ? c.bpm / 120 : 1.0))), "key-shifted preview keeps the matched duration");
        int crosses = 0; const auto& data = shifted.clip->left; const std::size_t begin = 4800, end = data.size() - 4800;
        for (std::size_t i = begin; i < end; ++i) { if (data[i-1] <= 0 && data[i] > 0) ++crosses; check(std::abs(data[i] + shifted.clip->right[i]) < 1e-5, "key shift keeps stereo phase linked"); }
        const double hz = crosses * 48000.0 / (end - begin), want = 440 * std::pow(2.0, c.semitones / 12.0);
        check(std::abs(hz - want) < want * .015, "key shift moves the 440 Hz pitch by the requested semitones");
    }
    check(alien::validPreviewSettings(true, 60, 120, 1) && !alien::validPreviewSettings(true, 60, 120, -1) && !alien::validPreviewSettings(false, 120, 120, 7)
          && !alien::validPreviewSettings(true, 240, 120, 1), "key shift and tempo ratio together stay inside the stretch range");
    check(!alien::renderSamplePreview(file, true, 60, 120, cancel, -1).clip, "too-far combined stretch refuses preview");
    cancel.store(true); check(!alien::renderSamplePreview(file, true, 180, 120, cancel).clip, "cancelled preview returns no partial asset");
    check(!alien::renderSamplePreview(file, false, 0, 0, cancel, 3).clip, "cancelled key-shifted preview returns no partial asset");
    file.loadFileAsData(after); check(before == after, "tempo preview preserves source file byte-for-byte"); file.deleteFile();
    cancel.store(false);auto unwritable=file.getChildFile("missing").getChildFile("take.wav");
    check(!alien::writeClipWav(unwritable,*take,cancel,error) && take->valid(),"failed write preserves take for retry");
    std::cout<<"Capture, device stop, recording WAV and pitch-preserving tempo tests passed\n";
}

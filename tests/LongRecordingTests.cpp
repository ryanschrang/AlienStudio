#include "DiskRecording.h"
#include "AudioClipImport.h"
#include "ToneEngine.h"
#include <iostream>
#include <new>
#include <condition_variable>
#include <mutex>

static thread_local std::size_t allocations{};
void* operator new(std::size_t n){++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
static void check(bool ok,const char* text){if(!ok){std::cerr<<text<<'\n';std::exit(1);}}
static void yield(){std::this_thread::sleep_for(std::chrono::milliseconds(1));}
struct FakeState {std::atomic<std::uint64_t> frames{};std::atomic<int> checks{};bool failWrite{},failCheckpoint{};std::thread::id producer;std::atomic<bool> wrongThread{};std::mutex lock;std::condition_variable gate;bool blocked{},release{};};
struct FakeOutput final:alien::RecordingOutput {
    explicit FakeOutput(std::shared_ptr<FakeState> s):state(std::move(s)){}
    bool write(const float* const*,int,int n) override {
        if(std::this_thread::get_id()==state->producer)state->wrongThread=true;
        if(state->blocked){std::unique_lock<std::mutex> lock(state->lock);state->gate.wait(lock,[&]{return state->release;});}
        if(state->failWrite)return false;state->frames+=n;return true;
    }
    bool checkpoint() override{++state->checks;return !state->failCheckpoint;}
    alien::RecordingResult finish(bool failure) override{alien::RecordingResult r;r.writtenFrames=state->frames.load();if(failure)r.error="simulated disk failure";return r;}
    std::shared_ptr<FakeState> state;
};
struct FailingFile final:alien::RecordingOutput {
    FailingFile(std::unique_ptr<alien::RecordingFile> out,bool checkpoint):file(std::move(out)),checkpointFailure(checkpoint){}
    bool write(const float* const* d,int c,int n) override {if(!checkpointFailure && calls++>0)return false;return file->write(d,c,n);}
    bool checkpoint() override{return !checkpointFailure && file->checkpoint();}
    alien::RecordingResult finish(bool failed) override{return file->finish(failed);}
    std::unique_ptr<alien::RecordingFile> file;bool checkpointFailure;int calls{};
};
struct VirtualLargeWav final:juce::InputStream {
    VirtualLargeWav():header(alien::RecordingFile::makeHeader(48000,2,1000000000ULL)){}
    juce::int64 getTotalLength() override{return 80+6000000000LL;}
    juce::int64 getPosition() override{return position;}
    bool setPosition(juce::int64 value) override {if(value<0 || value>getTotalLength())return false;position=value;return true;}
    bool isExhausted() override{return position==getTotalLength();}
    int read(void* dest,int requested) override {
        const int n=static_cast<int>(std::min<juce::int64>(requested,getTotalLength()-position));
        std::memset(dest,0,static_cast<std::size_t>(n));
        for(int i=0;i<n && position+i<80;++i)static_cast<std::uint8_t*>(dest)[i]=header[static_cast<std::size_t>(position+i)];
        position+=n;return n;
    }
    std::array<std::uint8_t,80> header;juce::int64 position{};
};
static void feed(alien::DiskRecording& take,const float* const* input,int channels,std::uint64_t frames) {
    for(std::uint64_t offset=0;offset<frames;){
        while(take.frames.load()-take.writtenFrames.load()>alien::RecordingQueue::defaultCapacity/2 && !take.done.load())yield();
        check(!take.done.load(),"long recording stopped before requested duration");
        const int n=static_cast<int>(std::min<std::uint64_t>(257,frames-offset));
        const auto before=allocations;take.append(input,channels,n);check(allocations==before,"capture append allocated");offset+=n;
    }
}
int main(){
    alien::RecordingQueue queue(8000,2,8);float l[]{0.25f,0.5f,NAN,-0.25f,0.75f},r[]{-0.5f,-0.25f,INFINITY,0.5f,-0.75f};const float* inputs[]{l,r};
    queue.append(inputs,2,5);int count=0;
    check(queue.drain([&](const float* const* data,int channels,int n){check(channels==2&&n==3&&data[0][2]==0&&data[1][2]==0,"sanitize channels");count+=n;return true;},3),"first ring drain");
    queue.append(inputs,2,5);
    while(queue.drain([&](const float* const*,int,int n){count+=n;return true;})){}
    check(count==10 && queue.frames==10 && queue.empty(),"ring wrap retains all samples");
    queue.append(nullptr,0,12);check(queue.done && queue.overrun && queue.frames==18,"overflow seals exact accepted prefix");
    while(queue.drain([&](const float* const* data,int channels,int n){for(int c=0;c<channels;++c)for(int i=0;i<n;++i)check(data[c][i]==0,"missing inputs are silent");return true;})){}

    const auto root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("AlienLongRecording","",false);root.createDirectory();
    const auto recovery=root.getChildFile("Recovery");const auto target=root.getChildFile("Two minutes.wav");juce::String error;
    auto take=alien::DiskRecording::start(target,48000,2,error,recovery);check(take!=nullptr,"create long recording");
    const auto memory=take->bufferBytes();check(memory==2*alien::RecordingQueue::defaultCapacity*sizeof(float),"recording buffer is fixed size");
    std::array<float,257> left{},right{};left.fill(0.25f);right.fill(-0.5f);const float* audio[]{left.data(),right.data()};
    // Exercise the real device callback and device-stop path without physical IO.
    auto engine=std::make_unique<ToneEngine>();engine->prepare(48000);engine->capture.store(take.get());std::array<float,257> silence{};float* out[]{silence.data()};
    const auto before=allocations;engine->audioDeviceIOCallbackWithContext(audio,2,out,1,257,{});check(allocations==before,"actual recording callback allocated");
    for(float value:silence)check(value==0,"recording unexpectedly enabled monitoring");
    feed(*take,audio,2,48000*125-257);engine->audioDeviceStopped();check(take->done,"device stop seals disk take");engine->capture.store(nullptr);
    auto result=take->finish();check(result.file==target && result.error.isEmpty() && result.writtenFrames==48000*125,"125-second recording completes exactly");
    check(take->bufferBytes()==memory && alien::RecordingFile::unfinished(recovery).isEmpty(),"duration does not grow buffer; finished journal removed");
    juce::AudioFormatManager formats;formats.registerBasicFormats();std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(target));
    check(reader && reader->lengthInSamples==48000*125 && reader->bitsPerSample==24 && reader->numChannels==2,"long WAV metadata");
    juce::AudioBuffer<float> last(2,1);check(reader->read(&last,0,1,reader->lengthInSamples-1,true,true),"last recorded frame readable");
    check(std::abs(last.getSample(0,0)-0.25f)<1e-6 && std::abs(last.getSample(1,0)+0.5f)<1e-6,"long WAV last frame preserves stereo");reader.reset();
    check(alien::decodeSessionAudioClip(target).clip!=nullptr,"normal long takes still load in Session");
    check(!alien::DiskRecording::start(target,48000,2,error,recovery),"existing destination rejected");
    check(!alien::DiskRecording::start(root.getChildFile("missing/Take.wav"),48000,2,error,recovery),"unwritable folder rejected");

    // One simulated hour at 8 kHz: constant memory, periodic checkpoints, writer
    // calls only on its own thread. Faster than real time, with FIFO backpressure.
    auto state=std::make_shared<FakeState>();state->producer=std::this_thread::get_id();
    alien::DiskRecording hour(8000,1,std::make_unique<FakeOutput>(state));feed(hour,audio,1,8000ULL*3600);
    const auto hourResult=hour.finish();check(hourResult.writtenFrames==8000ULL*3600 && state->checks>0 && !state->wrongThread,"hour-long stream and worker IO");
    check(hour.bufferBytes()==alien::RecordingQueue::defaultCapacity*sizeof(float),"hour-long memory bounded");

    for(bool checkpointFailure:{false,true}){
        auto failure=std::make_shared<FakeState>();failure->failWrite=!checkpointFailure;failure->failCheckpoint=checkpointFailure;
        alien::DiskRecording broken(8000,1,std::make_unique<FakeOutput>(failure));
        if(checkpointFailure)feed(broken,audio,1,8250);else broken.append(audio,1,257);
        const auto failed=broken.finish();check(failed.error.isNotEmpty(),"disk write/checkpoint error surfaced");
    }
    auto slow=std::make_shared<FakeState>();slow->blocked=true;alien::DiskRecording overrun(8000,1,std::make_unique<FakeOutput>(slow),8);
    overrun.append(audio,1,257);check(overrun.done,"stalled writer stops capture on overrun");
    {std::lock_guard<std::mutex> lock(slow->lock);slow->release=true;}slow->gate.notify_all();
    const auto prefix=overrun.finish();check(prefix.interrupted && prefix.writtenFrames==8 && prefix.error.contains("fell behind"),"overrun reports saved continuous prefix");

    // Simulate process death: payload reaches disk but the header still says 0.
    const auto original=root.getChildFile("Interrupted.wav");auto file=alien::RecordingFile::create(original,48000,2,error,recovery);
    check(file && file->write(audio,2,257),"write interrupted payload");
    auto activeJournals=alien::RecordingFile::unfinished(recovery);
    const auto activeRecovery=alien::RecordingFile::recover(activeJournals[0]);
    check(activeRecovery.error.contains("still active") && !original.exists(),"active writer lease prevents recovery from another instance");
    file.reset();
    auto journals=alien::RecordingFile::unfinished(recovery);check(journals.size()==1,"unfinished journal discoverable");
    original.replaceWithText("existing destination must survive");
    const auto recovered=alien::RecordingFile::recover(journals[0]);check(recovered.file.existsAsFile() && recovered.file!=original && recovered.writtenFrames==257,"recover stale WAV header to a new filename");
    check(original.loadFileAsString()=="existing destination must survive","recovery never replaces existing destination");
    check(alien::decodeSessionAudioClip(recovered.file).clip->frames==257,"recovered take decodes");

    // Retained real partial files remain recoverable after injected write and
    // checkpoint failures, not just after a simulated process interruption.
    for(bool checkpointFailure:{false,true}) {
        auto backend=alien::RecordingFile::create(root.getChildFile(checkpointFailure?"Flush failure.wav":"Write failure.wav"),8000,2,error,recovery);
        check(backend!=nullptr,"create failure test backend");
        alien::DiskRecording failedTake(8000,2,std::make_unique<FailingFile>(std::move(backend),checkpointFailure));
        for(int i=0;i<50;++i)failedTake.append(audio,2,257);
        const auto failed=failedTake.finish();
        check(failed.file==juce::File{} && failed.partialFile.existsAsFile() && failed.journal.existsAsFile() && failed.error.isNotEmpty(),"failed take retains partial WAV and journal");
        const auto repaired=alien::RecordingFile::recover(failed.journal);
        check(repaired.file.existsAsFile() && repaired.writtenFrames==failed.writtenFrames,"recover disk-failure prefix exactly");
    }

    auto mono=alien::DiskRecording::start(root.getChildFile("Odd mono.wav"),8000,1,error,recovery);mono->append(audio,1,17);const auto odd=mono->finish();
    check(odd.file.getSize()==80+17*3+1 && alien::decodeSessionAudioClip(odd.file).clip->frames==17,"odd-byte WAV padding");
    const auto collisionTarget=root.getChildFile("Collision.wav");auto collision=alien::DiskRecording::start(collisionTarget,8000,1,error,recovery);
    collision->append(audio,1,17);collisionTarget.replaceWithText("preserve collision");const auto collisionResult=collision->finish();
    check(collisionResult.file==juce::File{} && collisionResult.journal.existsAsFile() && collisionTarget.loadFileAsString()=="preserve collision","finalization never replaces a raced destination");
    const auto collisionRecovery=alien::RecordingFile::recover(collisionResult.journal);
    check(collisionRecovery.file.existsAsFile() && collisionRecovery.file!=collisionTarget && collisionRecovery.writtenFrames==17,"collided take can be recovered");
    const auto header=alien::RecordingFile::makeHeader(48000,2,1000000000ULL);
    check(std::memcmp(header.data(),"RF64",4)==0 && std::memcmp(header.data()+12,"ds64",4)==0,"4 GiB transition selects RF64 without moving payload");
    juce::WavAudioFormat wav;std::unique_ptr<juce::AudioFormatReader> large(wav.createReaderFor(new VirtualLargeWav(),true));
    check(large && large->lengthInSamples==1000000000LL && large->numChannels==2 && large->bitsPerSample==24,"RF64 header decodes beyond 4 GiB");
    check(large->read(&last,0,1,999999999LL,true,true) && last.getSample(0,0)==0,"RF64 final virtual frame reads correctly");
    take.reset();root.deleteRecursively();std::cout<<"Long recording, bounded memory, callback safety, disk failures, overrun, recovery and RF64 headers passed\n";
}

#pragma once
#include "RecordingQueue.h"
#include <juce_core/juce_core.h>
#include <array>
#include <thread>
#include <chrono>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 // JUCE removes the Windows RPC `small` macro before declaring an enum with
 // that name. This header is included after juce_core, so remove it again.
 #ifdef small
  #undef small
 #endif
#else
 #include <unistd.h>
 #include <fcntl.h>
 #include <sys/file.h>
#endif

namespace alien {
struct RecordingResult {
    juce::File file, partialFile, journal;
    std::uint64_t writtenFrames{};
    juce::String error;
    bool interrupted{};
};
// Injected output also permits deterministic disk failure/stall testing.
class RecordingOutput {
public:
    virtual ~RecordingOutput() = default;
    virtual bool write(const float* const*,int,int) = 0;
    virtual bool checkpoint() = 0;
    virtual RecordingResult finish(bool ioFailed) = 0;
};

// Handle-owned locks can be released by the disk thread after setup on another
// thread. Keep the zero-byte lock inode: unlinking it permits split-lock races.
class RecordingLease {
public:
    explicit RecordingLease(const juce::File& file) {
       #if JUCE_WINDOWS
        handle=::CreateFileW(file.getFullPathName().toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
       #else
        descriptor=::open(file.getFullPathName().toRawUTF8(),O_CREAT|O_RDWR,0600);
        if(descriptor>=0 && ::flock(descriptor,LOCK_EX|LOCK_NB)!=0){::close(descriptor);descriptor=-1;}
       #endif
    }
    ~RecordingLease() {
       #if JUCE_WINDOWS
        if(valid())::CloseHandle(handle);
       #else
        if(valid())::close(descriptor);
       #endif
    }
    bool valid() const {
       #if JUCE_WINDOWS
        return handle!=INVALID_HANDLE_VALUE;
       #else
        return descriptor>=0;
       #endif
    }
    RecordingLease(const RecordingLease&)=delete;
    RecordingLease& operator=(const RecordingLease&)=delete;
private:
   #if JUCE_WINDOWS
    HANDLE handle=INVALID_HANDLE_VALUE;
   #else
    int descriptor=-1;
   #endif
};

class RecordingFile final : public RecordingOutput {
public:
    static juce::File recoveryDirectory() {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Alien Studio/Recording Recovery");
    }
    static std::unique_ptr<RecordingFile> create(const juce::File& target,double rate,int channels,juce::String& error,
                                                juce::File recovery = recoveryDirectory()) {
        if(!std::isfinite(rate)||rate<8000||rate>192000||rate!=std::floor(rate)||channels<1||channels>2) {error="Unsupported recording format";return {};}
        if(target.exists() || !target.getParentDirectory().isDirectory()) {error="Choose a new WAV filename in an existing folder";return {};}
        auto out=std::unique_ptr<RecordingFile>(new RecordingFile(target,rate,channels));
        const auto id=juce::Uuid().toString();
        out->result.partialFile=target.getSiblingFile(target.getFileNameWithoutExtension()+".recording-"+id+".partial.wav");
        out->result.journal=recovery.getChildFile(id+".json");
        if(recovery.createDirectory().failed()){error="Cannot create recording recovery directory";return {};}
        out->lease=std::make_unique<RecordingLease>(recovery.getChildFile(id+".lock"));
        if(!out->lease->valid()){error="Cannot lock recording recovery file";return {};}
        auto metadata=juce::var(new juce::DynamicObject());auto* object=metadata.getDynamicObject();
        object->setProperty("format","AlienStudioRecording");object->setProperty("version",1);
        object->setProperty("id",id);object->setProperty("target",target.getFullPathName());
        object->setProperty("partial",out->result.partialFile.getFullPathName());
        if(!out->result.journal.replaceWithText(juce::JSON::toString(metadata))) {error="Cannot write recording recovery information";return {};}
        out->stream=out->result.partialFile.createOutputStream();
        if(!out->stream || out->stream->getStatus().failed() || !out->checkpoint()) {
            error="Cannot create recording WAV (check permissions and free disk space)";
            // No capture has started, so an empty failed setup can be removed.
            out->stream.reset();out->result.partialFile.deleteFile();out->result.journal.deleteFile();
            return {};
        }
        error.clear();return out;
    }
    bool write(const float* const* data,int channels,int count) override {
        if(channels!=channelCount || count<1 || count>4096)return false;
        auto* dest=encoded.data();
        for(int i=0;i<count;++i)for(int c=0;c<channels;++c) {
            const float sample=std::isfinite(data[c][i])?std::clamp(data[c][i],-1.0f,1.0f):0;
            const auto value=static_cast<std::int32_t>(std::clamp(std::llround(sample*8388608.0),-8388608LL,8388607LL));
            const auto bits=static_cast<std::uint32_t>(value);
            *dest++=static_cast<std::uint8_t>(bits);*dest++=static_cast<std::uint8_t>(bits>>8);*dest++=static_cast<std::uint8_t>(bits>>16);
        }
        if(!stream->write(encoded.data(),static_cast<std::size_t>(count*channels*3)))return false;
        result.writtenFrames+=static_cast<unsigned>(count);return true;
    }
    bool checkpoint() override {
        const auto header=makeHeader(sampleRate,channelCount,result.writtenFrames);
        const auto end=static_cast<juce::int64>(headerSize+result.writtenFrames*channelCount*3);
        if(!stream || !stream->setPosition(0) || !stream->write(header.data(),header.size()) || !stream->setPosition(end))return false;
        stream->flush();return stream->getStatus().wasOk();
    }
    RecordingResult finish(bool ioFailed) override {
        auto finishingLease=std::move(lease);
        if((result.writtenFrames*channelCount*3 & 1u)!=0 && !stream->writeByte(0))ioFailed=true;
        if(!checkpoint())ioFailed=true;
        stream.reset();
        if(ioFailed) {result.error="Disk write/flush failed. The unfinished WAV was retained; free disk space and use Recover takes.";return result;}
        if(result.writtenFrames==0){result.error="No input samples captured";result.partialFile.deleteFile();result.journal.deleteFile();return result;}
        if(!publishWithoutReplacing(result.partialFile,target)) {
            result.error="Could not publish the take (destination may already exist). The unfinished WAV is retained for recovery.";return result;
        }
        result.file=target;result.journal.deleteFile();return result;
    }
    // On-disk header has a reserved ds64 slot, allowing RIFF -> RF64 without
    // moving audio when a take passes 4 GiB. Payload is always 24-bit PCM.
    static constexpr std::uint64_t headerSize=80;
    static std::array<std::uint8_t,80> makeHeader(double rate,int channels,std::uint64_t frames) {
        std::array<std::uint8_t,80> h{};
        auto text=[&](int pos,const char* s){for(int i=0;i<4;++i)h[pos+i]=static_cast<std::uint8_t>(s[i]);};
        auto integer=[&](int pos,std::uint64_t value,int bytes){for(int i=0;i<bytes;++i)h[pos+i]=static_cast<std::uint8_t>(value>>(i*8));};
        const auto bytes=frames*static_cast<unsigned>(channels)*3;
        // RIFF includes an even-byte data pad, but data size excludes it.
        const auto riff=72+bytes+(bytes&1u);const bool large=riff>0xffffffffu;
        text(0,large?"RF64":"RIFF");integer(4,large?0xffffffffu:riff,4);text(8,"WAVE");
        text(12,large?"ds64":"JUNK");integer(16,28,4);
        if(large){integer(20,riff,8);integer(28,bytes,8);integer(36,frames,8);}
        text(48,"fmt ");integer(52,16,4);integer(56,1,2);integer(58,channels,2);integer(60,static_cast<std::uint64_t>(rate),4);
        integer(64,static_cast<std::uint64_t>(rate)*channels*3,4);integer(68,channels*3,2);integer(70,24,2);
        text(72,"data");integer(76,large?0xffffffffu:bytes,4);return h;
    }
    static juce::Array<juce::File> unfinished(juce::File recovery = recoveryDirectory()) {
        auto files=recovery.findChildFiles(juce::File::findFiles,false,"*.json");
        files.sort();return files;
    }
    static juce::String recoveryName(const juce::File& journal) {
        if(journal.getSize()<1 || journal.getSize()>16384)return journal.getFileName();
        const auto metadata=juce::JSON::parse(journal.loadFileAsString());
        if(auto* o=metadata.getDynamicObject())return o->getProperty("target").toString();return journal.getFileName();
    }
    static RecordingResult recover(const juce::File& journal) {
        RecordingResult result;result.journal=journal;result.interrupted=true;
        if(journal.getSize()<1 || journal.getSize()>16384){result.error="Invalid recording journal";return result;}
        const auto metadata=juce::JSON::parse(journal.loadFileAsString());auto* o=metadata.getDynamicObject();
        if(!o || o->getProperty("format").toString()!="AlienStudioRecording" || static_cast<int>(o->getProperty("version"))!=1) {result.error="Invalid recording journal";return result;}
        const auto targetPath=o->getProperty("target").toString(),partialPath=o->getProperty("partial").toString(),id=o->getProperty("id").toString();
        if(id!=journal.getFileNameWithoutExtension() || !juce::File::isAbsolutePath(targetPath) || !juce::File::isAbsolutePath(partialPath)) {result.error="Invalid recording recovery paths";return result;}
        RecordingLease recoveryLease(journal.getSiblingFile(id+".lock"));
        if(!recoveryLease.valid()){result.error="Take is still active in another instance";return result;}
        juce::File target(targetPath);result.partialFile=juce::File(partialPath);
        const auto expected=target.getFileNameWithoutExtension()+".recording-"+id+".partial.wav";
        if(result.partialFile.getParentDirectory()!=target.getParentDirectory() || result.partialFile.getFileName()!=expected) {result.error="Invalid recording recovery source";return result;}
        if(!result.partialFile.existsAsFile()){
            // A crash after publishing may leave only the journal. Never rewrite
            // an existing target or another file to try to 'repair' this case.
            result.error="Unfinished WAV is missing. Check the original destination; it may already have been saved.";return result;
        }
        std::array<std::uint8_t,80> header{};auto input=result.partialFile.createInputStream();
        if(!input || input->read(header.data(),80)!=80){result.error="Unfinished WAV header is incomplete";return result;}
        auto text=[&](int n,const char* s){return std::memcmp(header.data()+n,s,4)==0;};
        auto integer=[&](int n,int bytes){std::uint64_t value=0;for(int i=0;i<bytes;++i)value|=static_cast<std::uint64_t>(header[n+i])<<(8*i);return value;};
        const auto rate=integer(60,4);const auto channels=integer(58,2);
        if((!text(0,"RIFF")&&!text(0,"RF64")) || !text(8,"WAVE") || (!text(12,"JUNK")&&!text(12,"ds64")) || integer(16,4)!=28
            || !text(48,"fmt ") || integer(52,4)!=16 || integer(56,2)!=1 || channels<1 || channels>2 || rate<8000 || rate>192000
            || integer(68,2)!=channels*3 || integer(70,2)!=24 || !text(72,"data")) {result.error="Unsupported unfinished WAV header";return result;}
        const auto size=result.partialFile.getSize();input.reset();
        result.writtenFrames=static_cast<std::uint64_t>(size-headerSize)/(channels*3);
        if(result.writtenFrames==0){result.error="Unfinished WAV contains no complete audio frames";return result;}
        auto out=result.partialFile.createOutputStream();
        const auto bytes=result.writtenFrames*channels*3;
        const auto end=static_cast<juce::int64>(headerSize+bytes);
        const auto repaired=makeHeader(static_cast<double>(rate),static_cast<int>(channels),result.writtenFrames);
        if(!out || !out->setPosition(end) || out->truncate().failed() || ((bytes&1u)!=0 && !out->writeByte(0)) || !out->setPosition(0) || !out->write(repaired.data(),repaired.size())) {result.error="Could not repair WAV; file retained";return result;}
        out->flush();if(out->getStatus().failed()){result.error="Could not flush repaired WAV; file retained";return result;}out.reset();
        if(target.exists())target=target.getNonexistentSibling(false);
        if(!publishWithoutReplacing(result.partialFile,target)){result.error="Could not publish recovered WAV; file retained";return result;}
        result.file=target;result.journal.deleteFile();return result;
    }
private:
    RecordingFile(juce::File file,double rate,int channels):target(std::move(file)),sampleRate(rate),channelCount(channels){}
    static bool publishWithoutReplacing(const juce::File& from,const juce::File& to) {
       #if JUCE_WINDOWS
        return ::MoveFileW(from.getFullPathName().toWideCharPointer(),to.getFullPathName().toWideCharPointer())!=0;
       #else
        if(::link(from.getFullPathName().toRawUTF8(),to.getFullPathName().toRawUTF8())!=0)return false;
        from.deleteFile();return true;
       #endif
    }
    juce::File target;
    const double sampleRate;
    const int channelCount;
    RecordingResult result;
    std::unique_ptr<RecordingLease> lease;
    std::unique_ptr<juce::FileOutputStream> stream;
    std::array<std::uint8_t,4096*2*3> encoded{};
};

// The disk thread owns output. The device callback only appends to queue.
// Detach from the callback BEFORE finish/destruction; finish runs off the GUI.
class DiskRecording final : public AudioCaptureSink {
public:
    static std::shared_ptr<DiskRecording> start(const juce::File& target,double rate,int channels,juce::String& error,
                                               juce::File recovery=RecordingFile::recoveryDirectory()) {
        try {
            auto file=RecordingFile::create(target,rate,channels,error,recovery);if(!file)return {};
            return std::make_shared<DiskRecording>(rate,channels,std::move(file));
        }
        catch(const std::exception& e){error="Cannot start recording: "+juce::String(e.what());return {};}
    }
    DiskRecording(double rate,int channels,std::unique_ptr<RecordingOutput> destination,std::size_t capacity=RecordingQueue::defaultCapacity)
        : AudioCaptureSink(rate,channels),queue(rate,channels,capacity),output(std::move(destination)) {
        if(!output)throw std::invalid_argument("Recording output is required");
        worker=std::thread([this]{run();});
    }
    ~DiskRecording() override { done.store(true,std::memory_order_release);if(worker.joinable())worker.join(); }
    void append(const float* const* input,int count,int samples) noexcept override {
        if(done.load(std::memory_order_acquire))return;
        queue.append(input,count,samples);
        frames.store(queue.frames.load(std::memory_order_acquire),std::memory_order_release);level.store(queue.level.load());
        if(queue.done.load(std::memory_order_acquire))done.store(true,std::memory_order_release);
    }
    RecordingResult finish() {
        done.store(true,std::memory_order_release);if(worker.joinable())worker.join();return result;
    }
    std::size_t bufferBytes() const noexcept { return queue.memoryBytes(); }
    std::atomic<std::uint64_t> writtenFrames{};
private:
    void run() noexcept {
        bool failed=false;std::uint64_t checkpointFrames=0;
        try {
            for(;;) {
                const bool drained=queue.drain([&](const float* const* data,int channels,int count){
                    if(!output->write(data,channels,count)){failed=true;return false;}
                    writtenFrames.fetch_add(static_cast<unsigned>(count),std::memory_order_release);return true;
                });
                if(failed)break;
                const auto committed=writtenFrames.load(std::memory_order_acquire);
                if(committed-checkpointFrames>=static_cast<std::uint64_t>(sampleRate)) {
                    if(!output->checkpoint()){failed=true;break;}checkpointFrames=committed;
                }
                if(done.load(std::memory_order_acquire) && queue.empty())break;
                if(!drained)std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            done.store(true,std::memory_order_release);
            result=output->finish(failed);
            if(queue.overrun.load()) {result.interrupted=true;result.error+=" Recording stopped because disk writing fell behind. Only the continuous captured prefix was saved.";}
        } catch(const std::exception& e) {
            done.store(true,std::memory_order_release);
            try{result=output->finish(true);}catch(...){}
            result.error="Recording writer failed: "+juce::String(e.what())+". Unfinished files were retained.";
        } catch(...) {done.store(true,std::memory_order_release);try{result=output->finish(true);}catch(...){}result.error="Recording writer failed; unfinished files were retained.";}
    }
    RecordingQueue queue;
    std::unique_ptr<RecordingOutput> output;
    std::thread worker;
    RecordingResult result;
};
}

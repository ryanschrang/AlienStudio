#pragma once
#include "SessionAudioClip.h"
#include <memory>

namespace alien {
// Offline waveform-similarity overlap-add, with linked stereo alignment.
// Original implementation; see Driedger/Mueller 2016 TSM overview, section 3.
// A fixed-tempo render, not a warp-marker editor or transient-aware algorithm.
inline std::shared_ptr<SessionAudioClip> warpTempo(const SessionAudioClip& input,
    double ratio,const std::atomic<bool>& cancelled) try {
    if(!input.valid() || !std::isfinite(ratio) || ratio<0.5 || ratio>2
        || cancelled.load()) return {};
    const auto frames=static_cast<std::size_t>(std::ceil(input.frames*ratio));
    if(frames>SessionAudioClip::maxDecodedBytes/(input.channels*sizeof(float))) return {};
    auto out=std::make_shared<SessionAudioClip>();
    out->sampleRate=input.sampleRate;out->channels=input.channels;out->frames=frames;
    out->left.resize(frames);if(input.channels==2)out->right.resize(frames);
    if(ratio==1) { out->left=input.left;out->right=input.right;out->buildWaveform();return out; }
    const int grain=std::max(64,static_cast<int>(input.sampleRate*0.04));
    const int hop=grain/4,seek=std::max(8,static_cast<int>(input.sampleRate*0.006));
    std::vector<float> weights(frames),window(static_cast<std::size_t>(grain));
    for(int n=0;n<grain;++n)window[n]=static_cast<float>(0.5-0.5*std::cos(6.283185307179586*(n+0.5)/grain));
    auto sample=[&](const std::vector<float>& channel,int index) { return index>=0 && static_cast<std::size_t>(index)<input.frames?channel[static_cast<std::size_t>(index)]:0.0f; };
    int previous=0;
    for(std::size_t pos=0;pos<frames;pos+=static_cast<std::size_t>(hop)) {
        if(cancelled.load())return {};
        const int expected=static_cast<int>(pos/ratio);
        int best=expected;
        if(pos>0) {
            double bestScore=-2;
            // Summing per-channel correlations avoids cancellation of anti-phase stereo.
            for(int candidate=std::max(0,expected-seek);candidate<=expected+seek;++candidate) {
                double dot=0,aa=0,bb=0;
                for(int n=0;n<grain-hop;n+=8) {
                    const double a=sample(input.left,previous+hop+n),b=sample(input.left,candidate+n);
                    dot+=a*b;aa+=a*a;bb+=b*b;
                    if(input.channels==2) { const double ar=sample(input.right,previous+hop+n),br=sample(input.right,candidate+n);dot+=ar*br;aa+=ar*ar;bb+=br*br; }
                }
                const double score=dot/std::sqrt(aa*bb+1.0e-20)-1.0e-6*std::abs(candidate-expected);
                if(score>bestScore){bestScore=score;best=candidate;}
            }
        }
        for(int n=0;n<grain && pos+static_cast<std::size_t>(n)<frames;++n) {
            const auto i=pos+static_cast<std::size_t>(n);const float w=window[n];
            out->left[i]+=w*sample(input.left,best+n);
            if(input.channels==2)out->right[i]+=w*sample(input.right,best+n);
            weights[i]+=w;
        }
        previous=best;
    }
    for(std::size_t i=0;i<frames;++i)if(weights[i]>0){out->left[i]/=weights[i];if(input.channels==2)out->right[i]/=weights[i];}
    out->buildWaveform();return out;
} catch(const std::bad_alloc&) { return {}; }
} // namespace alien

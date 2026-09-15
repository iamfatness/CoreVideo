#pragma once
#include "iso-audio-reader.h"
#include <memory>

class IsoAudioTap : public IsoAudioReader, public std::enable_shared_from_this<IsoAudioTap> {
public:
    using IsoAudioReader::IsoAudioReader;
    ~IsoAudioTap();
    bool start();
    void stop();
};

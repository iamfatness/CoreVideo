/**
 * Global raw-data type definitions used by the Zoom SDK callbacks.
 * YUVRawDataI420 and AudioRawData are forward-declared in the rawdata headers
 * but defined in a separate SDK file not distributed publicly.
 * These minimal abstract interfaces match the SDK ABI.
 */
#pragma once
#include <cstdint>

class YUVRawDataI420 {
public:
    virtual bool      CanAddRef()          = 0;
    virtual bool      AddRef()             = 0;
    virtual int       Release()            = 0;
    virtual char     *GetYBuffer()      = 0;
    virtual char     *GetUBuffer()      = 0;
    virtual char     *GetVBuffer()      = 0;
    virtual char     *GetAlphaBuffer()  = 0;
    virtual char     *GetBuffer()       = 0;
    virtual uint32_t  GetBufferLen()    = 0;
    virtual uint32_t  GetAlphaBufferLen() = 0;
    virtual bool      IsLimitedI420()   = 0;
    virtual uint32_t  GetStreamWidth()  = 0;
    virtual uint32_t  GetStreamHeight() = 0;
    virtual uint32_t  GetRotation()     = 0;
    virtual uint32_t  GetSourceID()     = 0;
    virtual uint64_t  GetTimeStamp()    = 0;
    virtual ~YUVRawDataI420() {}
};

class AudioRawData {
public:
    virtual bool      CanAddRef()     = 0;
    virtual bool      AddRef()        = 0;
    virtual int       Release()       = 0;
    virtual char     *GetBuffer()     = 0;
    virtual uint32_t  GetBufferLen()  = 0;
    virtual uint32_t  GetSampleRate() = 0;
    virtual uint32_t  GetChannelNum() = 0;
    virtual uint64_t  GetTimeStamp()  = 0;
    virtual ~AudioRawData() {}
};

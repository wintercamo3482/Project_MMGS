#pragma once

// File Name:
//   MediaConverter.h
//
// Description:
//   Declares Media Foundation audio extraction for MP3 and WAV.

#include <functional>
#include <string>

namespace mmgs
{
    using ProgressCallback = std::function<void(int)>;
    using LogCallback = std::function<void(const std::wstring&)>;

    enum class AudioFormat
    {
        Mp3,
        Wav
    };

    // Called on a worker thread. The destination is replaced only after success.
    bool ConvertMediaToAudio(
        const std::wstring& RO_source,
        const std::wstring& RO_destination,
        AudioFormat o_format,
        const ProgressCallback& RO_on_progress,
        const LogCallback& RO_on_log,
        std::wstring& o_error);
}

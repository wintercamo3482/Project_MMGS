#pragma once

// File Name:
//   MediaConverter.h
//
// Description:
//   Declares the Windows Media Foundation MP3 conversion interface.

#include <functional>
#include <string>

namespace mmgs
{
    using ProgressCallback = std::function<void(int)>;
    using LogCallback = std::function<void(const std::wstring&)>;

    // Called on a worker thread. The destination is replaced only after success.
    bool ConvertMediaToMp3(
        const std::wstring& RO_source,
        const std::wstring& RO_destination,
        const ProgressCallback& RO_on_progress,
        const LogCallback& RO_on_log,
        std::wstring& o_error);
}
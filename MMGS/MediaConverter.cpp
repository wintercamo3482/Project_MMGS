// File Name:
//   MediaConverter.cpp
//
// Description:
//   Decodes media audio with Media Foundation; outputs MP3 or PCM WAV.
//   No FFmpeg executable or third-party codec is required.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propidl.h>
#include <wrl/client.h>

#include "MediaConverter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")

#define MMGS_HNS_PER_SECOND 10000000LL
#define MMGS_PCM_BITS_PER_SAMPLE 16U
#define MMGS_PCM_BYTES_PER_SAMPLE 2U
#define MMGS_MP3_RATE_48KHZ 48000U
#define MMGS_MP3_RATE_44KHZ 44100U
#define MMGS_MP3_RATE_32KHZ 32000U
#define MMGS_AUDIO_MONO 1U
#define MMGS_AUDIO_STEREO 2U
#define MMGS_MP3_MONO_BYTES_PER_SECOND 16000U
#define MMGS_MP3_STEREO_BYTES_PER_SECOND 24000U
#define MMGS_MP3_MONO_KBPS 128U
#define MMGS_MP3_STEREO_KBPS 192U
#define MMGS_PROGRESS_SCALE 100
#define MMGS_PROGRESS_BEFORE_FINALIZE 99
#define MMGS_PROGRESS_LOG_INTERVAL 10
#define MMGS_MIN_SAMPLE_DURATION 1LL

using Microsoft::WRL::ComPtr;

namespace mmgs
{
    namespace
    {
        struct FoundationLifetime
        {
            bool mb1_com_initialized = false;
            bool mb1_mf_started = false;

            ~FoundationLifetime()
            {
                if (mb1_mf_started)
                {
                    MFShutdown();
                }

                if (mb1_com_initialized)
                {
                    CoUninitialize();
                }
            }
        };

        struct WaveFile
        {
            HANDLE mo_handle = INVALID_HANDLE_VALUE;
            uint64_t u64_data_bytes = 0;

            ~WaveFile()
            {
                if (mo_handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(mo_handle);
                }
            }

            void Close()
            {
                if (mo_handle != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(mo_handle);
                    mo_handle = INVALID_HANDLE_VALUE;
                }
            }
        };

        struct TempFileCleanup
        {
            std::wstring mo_path;

            ~TempFileCleanup()
            {
                if (!mo_path.empty())
                {
                    DeleteFileW(mo_path.c_str());
                }
            }
        };

        void Log(const LogCallback& RO_callback, const std::wstring& RO_message)
        {
            if (RO_callback)
            {
                RO_callback(RO_message);
            }
        }

        std::wstring HexHRESULT(HRESULT s32_hr)
        {
            wchar_t a16_buffer[16]{};
            swprintf_s(a16_buffer, L"0x%08X", static_cast<unsigned int>(s32_hr));
            std::wstring o_ret = a16_buffer;
            return o_ret;
        }

        bool CheckHResult(HRESULT s32_hr, const wchar_t* RO_step,
            std::wstring& o_error)
        {
            bool b1_ret = false;

            if (SUCCEEDED(s32_hr))
            {
                b1_ret = true;
            }
            else
            {
                o_error = std::wstring(RO_step) + L" failed (HRESULT " +
                    HexHRESULT(s32_hr) + L").";
            }

            return b1_ret;
        }

        bool FileHasData(const std::wstring& RO_path)
        {
            bool b1_ret = false;
            WIN32_FILE_ATTRIBUTE_DATA o_attributes{};

            if (!GetFileAttributesExW(
                RO_path.c_str(), GetFileExInfoStandard, &o_attributes))
            {
                return b1_ret;
            }

            if ((o_attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return b1_ret;
            }

            if (o_attributes.nFileSizeHigh != 0 || o_attributes.nFileSizeLow != 0)
            {
                b1_ret = true;
            }

            return b1_ret;
        }

        // Temporary file stays on the destination volume for atomic replacement.
        std::wstring MakeTempPath(const std::wstring& RO_destination,
            AudioFormat o_format)
        {
            std::wstring o_ret = RO_destination + L".mmgs-" +
                std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetCurrentThreadId()) + L"-" +
                std::to_wstring(GetTickCount64()) +
                (o_format == AudioFormat::Wav ? L".wav" : L".mp3");
            return o_ret;
        }

        // WAV files use a standard 44-byte RIFF/WAVE header and 16-bit PCM.
        // RIFF stores sizes in 32-bit fields; reject >4 GiB rather than corrupting output.
        void SetLe16(std::array<BYTE, 44>& o_header, size_t u64_offset, uint16_t u16_value)
        {
            o_header[u64_offset] = static_cast<BYTE>(u16_value & 0xFF);
            o_header[u64_offset + 1] = static_cast<BYTE>((u16_value >> 8) & 0xFF);
        }

        void SetLe32(std::array<BYTE, 44>& o_header, size_t u64_offset, uint32_t u32_value)
        {
            for (size_t u64_i = 0; u64_i < 4; ++u64_i)
            {
                o_header[u64_offset + u64_i] =
                    static_cast<BYTE>((u32_value >> (u64_i * 8)) & 0xFF);
            }
        }

        bool WriteWaveHeader(HANDLE o_handle, UINT32 u32_rate, UINT32 u32_channels,
            uint32_t u32_data_bytes, std::wstring& o_error)
        {
            bool b1_ret = false;
            std::array<BYTE, 44> o_header{};
            o_header[0] = 'R'; o_header[1] = 'I';
            o_header[2] = 'F'; o_header[3] = 'F';
            o_header[8] = 'W'; o_header[9] = 'A';
            o_header[10] = 'V'; o_header[11] = 'E';
            o_header[12] = 'f'; o_header[13] = 'm';
            o_header[14] = 't'; o_header[15] = ' ';
            o_header[36] = 'd'; o_header[37] = 'a';
            o_header[38] = 't'; o_header[39] = 'a';

            SetLe32(o_header, 4, 36U + u32_data_bytes);
            SetLe32(o_header, 16, 16U); // PCM fmt chunk size
            SetLe16(o_header, 20, 1U);  // WAVE_FORMAT_PCM
            SetLe16(o_header, 22, static_cast<uint16_t>(u32_channels));
            SetLe32(o_header, 24, u32_rate);
            SetLe32(o_header, 28, u32_rate * u32_channels * MMGS_PCM_BYTES_PER_SAMPLE);
            SetLe16(o_header, 32, static_cast<uint16_t>(u32_channels * MMGS_PCM_BYTES_PER_SAMPLE));
            SetLe16(o_header, 34, MMGS_PCM_BITS_PER_SAMPLE);
            SetLe32(o_header, 40, u32_data_bytes);

            LARGE_INTEGER o_position{};
            if (!SetFilePointerEx(o_handle, o_position, nullptr, FILE_BEGIN))
            {
                o_error = L"Could not seek to WAV header (Win32 error " +
                    std::to_wstring(GetLastError()) + L").";
                return b1_ret;
            }

            DWORD u32_written = 0;
            if (!WriteFile(o_handle, o_header.data(),
                static_cast<DWORD>(o_header.size()), &u32_written, nullptr) ||
                u32_written != o_header.size())
            {
                o_error = L"Could not write WAV header (Win32 error " +
                    std::to_wstring(GetLastError()) + L").";
                return b1_ret;
            }

            b1_ret = true;
            return b1_ret;
        }

        bool StartWaveFile(const std::wstring& RO_path, UINT32 u32_rate,
            UINT32 u32_channels, WaveFile& o_file,
            std::wstring& o_error)
        {
            bool b1_ret = false;
            o_file.mo_handle = CreateFileW(RO_path.c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (o_file.mo_handle == INVALID_HANDLE_VALUE)
            {
                o_error = L"Could not create WAV output (Win32 error " +
                    std::to_wstring(GetLastError()) + L").";
                return b1_ret;
            }

            b1_ret = WriteWaveHeader(o_file.mo_handle, u32_rate, u32_channels, 0, o_error);
            return b1_ret;
        }

        bool WriteWaveSample(IMFSample* o_sample, WaveFile& o_file,
            UINT32 u32_channels, std::wstring& o_error)
        {
            bool b1_ret = false;
            ComPtr<IMFMediaBuffer> o_buffer;
            HRESULT s32_hr = o_sample->ConvertToContiguousBuffer(o_buffer.GetAddressOf());
            if (!CheckHResult(s32_hr, L"Convert decoded audio buffer", o_error))
            {
                return b1_ret;
            }

            BYTE* a_buffer = nullptr;
            DWORD u32_max_length = 0;
            DWORD u32_data_length = 0;
            s32_hr = o_buffer->Lock(&a_buffer, &u32_max_length, &u32_data_length);
            if (!CheckHResult(s32_hr, L"Lock decoded audio buffer", o_error))
            {
                return b1_ret;
            }

            const DWORD u32_frame_size = u32_channels * MMGS_PCM_BYTES_PER_SAMPLE;
            if (u32_data_length % u32_frame_size != 0)
            {
                o_error = L"Decoded PCM sample is not aligned to an audio frame.";
            }
            else if (o_file.u64_data_bytes + u32_data_length > 0xFFFFFFFFULL - 36ULL)
            {
                o_error = L"WAV exceeds the 4 GiB RIFF limit. Split the input media.";
            }
            else
            {
                DWORD u32_written = 0;
                if (WriteFile(o_file.mo_handle, a_buffer, u32_data_length,
                    &u32_written, nullptr) && u32_written == u32_data_length)
                {
                    o_file.u64_data_bytes += u32_written;
                    b1_ret = true;
                }
                else
                {
                    o_error = L"Could not write WAV audio data (Win32 error " +
                        std::to_wstring(GetLastError()) + L").";
                }
            }

            o_buffer->Unlock();
            return b1_ret;
        }

        bool FinishWaveFile(WaveFile& o_file, UINT32 u32_rate,
            UINT32 u32_channels, std::wstring& o_error)
        {
            bool b1_ret = false;
            if (o_file.u64_data_bytes == 0)
            {
                o_error = L"No decoded audio was written to the WAV file.";
                return b1_ret;
            }

            if (!WriteWaveHeader(o_file.mo_handle, u32_rate, u32_channels,
                static_cast<uint32_t>(o_file.u64_data_bytes), o_error))
            {
                return b1_ret;
            }

            if (!FlushFileBuffers(o_file.mo_handle))
            {
                o_error = L"Could not flush WAV output (Win32 error " +
                    std::to_wstring(GetLastError()) + L").";
                return b1_ret;
            }

            o_file.Close();
            b1_ret = true;
            return b1_ret;
        }

        HRESULT CreatePcmType(UINT32 u32_rate, UINT32 u32_channels,
            IMFMediaType** o_result)
        {
            HRESULT s32_ret = E_POINTER;

            if (o_result == nullptr)
            {
                return s32_ret;
            }

            *o_result = nullptr;
            ComPtr<IMFMediaType> o_type;
            s32_ret = MFCreateMediaType(o_type.GetAddressOf());

            if (FAILED(s32_ret))
            {
                return s32_ret;
            }

            s32_ret = o_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);

            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, u32_rate);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, u32_channels);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(
                    MF_MT_AUDIO_BITS_PER_SAMPLE, MMGS_PCM_BITS_PER_SAMPLE);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                    u32_channels * MMGS_PCM_BYTES_PER_SAMPLE);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                    u32_rate * u32_channels * MMGS_PCM_BYTES_PER_SAMPLE);
            }
            if (SUCCEEDED(s32_ret))
            {
                *o_result = o_type.Detach();
            }

            return s32_ret;
        }

        HRESULT CreateMp3Type(UINT32 u32_rate, UINT32 u32_channels,
            IMFMediaType** o_result)
        {
            HRESULT s32_ret = E_POINTER;

            if (o_result == nullptr)
            {
                return s32_ret;
            }

            *o_result = nullptr;
            ComPtr<IMFMediaType> o_type;
            s32_ret = MFCreateMediaType(o_type.GetAddressOf());

            if (FAILED(s32_ret))
            {
                return s32_ret;
            }

            s32_ret = o_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);

            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, u32_rate);
            }
            if (SUCCEEDED(s32_ret))
            {
                s32_ret = o_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, u32_channels);
            }
            if (SUCCEEDED(s32_ret))
            {
                const UINT32 u32_bytes_per_second =
                    (u32_channels == MMGS_AUDIO_MONO) ?
                    MMGS_MP3_MONO_BYTES_PER_SECOND : MMGS_MP3_STEREO_BYTES_PER_SECOND;
                s32_ret = o_type->SetUINT32(
                    MF_MT_AUDIO_AVG_BYTES_PER_SECOND, u32_bytes_per_second);
            }
            if (SUCCEEDED(s32_ret))
            {
                *o_result = o_type.Detach();
            }

            return s32_ret;
        }

        void AddRateIfMissing(std::vector<UINT32>& o_rates, UINT32 u32_rate)
        {
            if (std::find(o_rates.begin(), o_rates.end(), u32_rate) == o_rates.end())
            {
                o_rates.push_back(u32_rate);
            }
        }

        HRESULT ConfigureReader(IMFSourceReader* o_reader, UINT32& u32_rate,
            UINT32& u32_channels)
        {
            HRESULT s32_ret = o_reader->SetStreamSelection(
                MF_SOURCE_READER_ALL_STREAMS, FALSE);

            if (FAILED(s32_ret))
            {
                return s32_ret;
            }

            s32_ret = o_reader->SetStreamSelection(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

            if (FAILED(s32_ret))
            {
                return s32_ret;
            }

            ComPtr<IMFMediaType> o_native_type;
            UINT32 u32_native_rate = 0;
            UINT32 u32_native_channels = 0;

            if (SUCCEEDED(o_reader->GetNativeMediaType(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                o_native_type.GetAddressOf())))
            {
                o_native_type->GetUINT32(
                    MF_MT_AUDIO_SAMPLES_PER_SECOND, &u32_native_rate);
                o_native_type->GetUINT32(
                    MF_MT_AUDIO_NUM_CHANNELS, &u32_native_channels);
            }

            std::vector<UINT32> o_rates;

            if (u32_native_rate == MMGS_MP3_RATE_48KHZ ||
                u32_native_rate == MMGS_MP3_RATE_44KHZ ||
                u32_native_rate == MMGS_MP3_RATE_32KHZ)
            {
                AddRateIfMissing(o_rates, u32_native_rate);
            }

            AddRateIfMissing(o_rates, MMGS_MP3_RATE_48KHZ);
            AddRateIfMissing(o_rates, MMGS_MP3_RATE_44KHZ);
            AddRateIfMissing(o_rates, MMGS_MP3_RATE_32KHZ);

            const UINT32 u32_first_channels =
                (u32_native_channels == MMGS_AUDIO_MONO) ?
                MMGS_AUDIO_MONO : MMGS_AUDIO_STEREO;
            const UINT32 u32_other_channels =
                (u32_first_channels == MMGS_AUDIO_MONO) ?
                MMGS_AUDIO_STEREO : MMGS_AUDIO_MONO;

            for (UINT32 u32_candidate_channels : {u32_first_channels, u32_other_channels})
            {
                for (UINT32 u32_candidate_rate : o_rates)
                {
                    ComPtr<IMFMediaType> o_pcm_type;
                    s32_ret = CreatePcmType(u32_candidate_rate,
                        u32_candidate_channels, o_pcm_type.GetAddressOf());

                    if (FAILED(s32_ret))
                    {
                        return s32_ret;
                    }

                    s32_ret = o_reader->SetCurrentMediaType(
                        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                        o_pcm_type.Get());

                    if (SUCCEEDED(s32_ret))
                    {
                        u32_rate = u32_candidate_rate;
                        u32_channels = u32_candidate_channels;
                        return s32_ret;
                    }
                }
            }

            return s32_ret;
        }

        bool ConvertCore(const std::wstring& RO_source,
            const std::wstring& RO_temporary,
            AudioFormat o_format,
            const ProgressCallback& RO_on_progress,
            const LogCallback& RO_on_log,
            std::wstring& o_error)
        {
            bool b1_ret = false;
            FoundationLifetime o_lifetime;
            HRESULT s32_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

            if (!CheckHResult(s32_hr, L"CoInitializeEx", o_error))
            {
                return b1_ret;
            }

            o_lifetime.mb1_com_initialized = true;
            s32_hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);

            if (!CheckHResult(s32_hr, L"MFStartup", o_error))
            {
                return b1_ret;
            }

            o_lifetime.mb1_mf_started = true;
            Log(RO_on_log, L"Opening input via Media Foundation Source Reader...");

            ComPtr<IMFSourceReader> o_reader;
            s32_hr = MFCreateSourceReaderFromURL(
                RO_source.c_str(), nullptr, o_reader.GetAddressOf());

            if (!CheckHResult(s32_hr, L"Open source", o_error))
            {
                return b1_ret;
            }

            UINT32 u32_rate = 0;
            UINT32 u32_channels = 0;
            s32_hr = ConfigureReader(o_reader.Get(), u32_rate, u32_channels);

            if (!CheckHResult(s32_hr, L"Find/decode audio (16-bit PCM)", o_error))
            {
                o_error += L"\nThis file may have no audio, or its codec may not be supported by Windows.";
                return b1_ret;
            }

            Log(RO_on_log, L"PCM: " + std::to_wstring(u32_rate) + L" Hz, " +
                std::to_wstring(u32_channels) + L" channel(s), 16-bit");

            PROPVARIANT o_prop;
            PropVariantInit(&o_prop);
            LONGLONG s64_duration = 0;
            s32_hr = o_reader->GetPresentationAttribute(
                MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &o_prop);

            if (SUCCEEDED(s32_hr))
            {
                if (o_prop.vt == VT_UI8)
                {
                    s64_duration = static_cast<LONGLONG>(o_prop.uhVal.QuadPart);
                }
                else if (o_prop.vt == VT_I8)
                {
                    s64_duration = o_prop.hVal.QuadPart;
                }
            }

            PropVariantClear(&o_prop);

            if (s64_duration > 0)
            {
                Log(RO_on_log, L"Duration: " +
                    std::to_wstring(s64_duration / MMGS_HNS_PER_SECOND) + L" sec");
            }
            else
            {
                Log(RO_on_log, L"Duration unavailable: progress will complete at the end.");
            }

            ComPtr<IMFSinkWriter> o_writer;
            WaveFile o_wave_file;
            DWORD u32_stream_index = 0;

            if (o_format == AudioFormat::Wav)
            {
                Log(RO_on_log, L"Writing WAV (16-bit PCM, uncompressed)...");
                if (!StartWaveFile(RO_temporary, u32_rate, u32_channels,
                    o_wave_file, o_error))
                {
                    return b1_ret;
                }
            }
            else
            {
                Log(RO_on_log, L"Configuring MP3 sink writer...");
                s32_hr = MFCreateSinkWriterFromURL(
                    RO_temporary.c_str(), nullptr, nullptr, o_writer.GetAddressOf());
                if (!CheckHResult(s32_hr, L"Create MP3 sink writer", o_error))
                {
                    return b1_ret;
                }

                ComPtr<IMFMediaType> o_mp3_type;
                s32_hr = CreateMp3Type(u32_rate, u32_channels, o_mp3_type.GetAddressOf());
                if (!CheckHResult(s32_hr, L"Set MP3 format", o_error))
                {
                    return b1_ret;
                }

                s32_hr = o_writer->AddStream(o_mp3_type.Get(), &u32_stream_index);
                if (!CheckHResult(s32_hr, L"Add MP3 output stream", o_error))
                {
                    return b1_ret;
                }

                ComPtr<IMFMediaType> o_pcm_type;
                s32_hr = o_reader->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, o_pcm_type.GetAddressOf());
                if (!CheckHResult(s32_hr, L"Get decoded PCM type", o_error))
                {
                    return b1_ret;
                }

                s32_hr = o_writer->SetInputMediaType(
                    u32_stream_index, o_pcm_type.Get(), nullptr);
                if (!CheckHResult(s32_hr, L"Connect Windows MP3 encoder", o_error))
                {
                    o_error += L"\nThe Windows MP3 encoder may be unavailable or incompatible.";
                    return b1_ret;
                }

                s32_hr = o_writer->BeginWriting();
                if (!CheckHResult(s32_hr, L"Start MP3 output", o_error))
                {
                    return b1_ret;
                }

                Log(RO_on_log, L"Encoding MP3 (" +
                    std::to_wstring(u32_channels == MMGS_AUDIO_MONO ?
                        MMGS_MP3_MONO_KBPS : MMGS_MP3_STEREO_KBPS) + L" kbps CBR)...");
            }

            bool b1_have_base_time = false;
            LONGLONG s64_first_time = 0;
            LONGLONG s64_next_write_time = 0;
            int32_t s32_last_percent = -1;
            uint64_t u64_sample_count = 0;

            for (;;)
            {
                DWORD u32_flags = 0;
                LONGLONG s64_timestamp = 0;
                ComPtr<IMFSample> o_sample;
                s32_hr = o_reader->ReadSample(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                    &u32_flags, &s64_timestamp, o_sample.GetAddressOf());

                if (!CheckHResult(s32_hr, L"Read decoded audio sample", o_error))
                {
                    return b1_ret;
                }

                if ((u32_flags & MF_SOURCE_READERF_ERROR) != 0)
                {
                    o_error = L"Source Reader reported a media decoding error.";
                    return b1_ret;
                }

                if ((u32_flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                    MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0)
                {
                    o_error = L"Audio format changed during conversion (not supported yet).";
                    return b1_ret;
                }

                if ((u32_flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                {
                    break;
                }

                if (!o_sample)
                {
                    continue;
                }

                DWORD u32_bytes = 0;
                s32_hr = o_sample->GetTotalLength(&u32_bytes);

                if (!CheckHResult(s32_hr, L"Get decoded sample size", o_error))
                {
                    return b1_ret;
                }

                if (u32_bytes == 0)
                {
                    continue;
                }

                if (!b1_have_base_time)
                {
                    s64_first_time = s64_timestamp;
                    b1_have_base_time = true;
                }

                LONGLONG s64_write_time = std::max<LONGLONG>(
                    0, s64_timestamp - s64_first_time);
                s64_write_time = std::max(s64_write_time, s64_next_write_time);

                LONGLONG s64_sample_duration = 0;

                if (FAILED(o_sample->GetSampleDuration(&s64_sample_duration)) ||
                    s64_sample_duration <= 0)
                {
                    const UINT32 u32_bytes_per_frame =
                        u32_channels * MMGS_PCM_BYTES_PER_SAMPLE;
                    s64_sample_duration =
                        (static_cast<LONGLONG>(u32_bytes / u32_bytes_per_frame) *
                            MMGS_HNS_PER_SECOND) / u32_rate;
                }

                if (s64_sample_duration <= 0)
                {
                    s64_sample_duration = MMGS_MIN_SAMPLE_DURATION;
                }

                if (o_format == AudioFormat::Wav)
                {
                    if (!WriteWaveSample(o_sample.Get(), o_wave_file,
                        u32_channels, o_error))
                    {
                        return b1_ret;
                    }
                }
                else
                {
                    s32_hr = o_sample->SetSampleTime(s64_write_time);
                    if (SUCCEEDED(s32_hr))
                    {
                        s32_hr = o_sample->SetSampleDuration(s64_sample_duration);
                    }
                    if (SUCCEEDED(s32_hr))
                    {
                        s32_hr = o_writer->WriteSample(u32_stream_index, o_sample.Get());
                    }
                    if (!CheckHResult(s32_hr, L"Encode/write MP3 sample", o_error))
                    {
                        return b1_ret;
                    }
                }

                ++u64_sample_count;
                s64_next_write_time = s64_write_time + s64_sample_duration;

                if (s64_duration > 0)
                {
                    const int32_t s32_progress = static_cast<int32_t>(
                        std::min<LONGLONG>(MMGS_PROGRESS_BEFORE_FINALIZE,
                            (std::max<LONGLONG>(0, s64_timestamp) *
                                MMGS_PROGRESS_SCALE) / s64_duration));

                    if (s32_progress > s32_last_percent)
                    {
                        s32_last_percent = s32_progress;

                        if (RO_on_progress)
                        {
                            RO_on_progress(s32_progress);
                        }

                        if (s32_progress % MMGS_PROGRESS_LOG_INTERVAL == 0 ||
                            s32_progress == MMGS_PROGRESS_BEFORE_FINALIZE)
                        {
                            Log(RO_on_log, L"Progress: " +
                                std::to_wstring(s32_progress) + L"%");
                        }
                    }
                }
            }

            if (u64_sample_count == 0)
            {
                o_error = L"The input contained no decodable audio samples.";
                return b1_ret;
            }

            if (o_format == AudioFormat::Wav)
            {
                if (!FinishWaveFile(o_wave_file, u32_rate, u32_channels, o_error))
                {
                    return b1_ret;
                }
            }
            else
            {
                s32_hr = o_writer->Finalize();
                if (!CheckHResult(s32_hr, L"Finalize MP3 file", o_error))
                {
                    return b1_ret;
                }
                o_writer.Reset();
            }

            o_reader.Reset();
            if (!FileHasData(RO_temporary))
            {
                o_error = L"Output audio file was empty after finalization.";
                return b1_ret;
            }

            Log(RO_on_log, L"Processed audio samples: " +
                std::to_wstring(u64_sample_count));
            b1_ret = true;
            return b1_ret;
        }
    }

    bool ConvertMediaToAudio(const std::wstring& RO_source,
        const std::wstring& RO_destination,
        AudioFormat o_format,
        const ProgressCallback& RO_on_progress,
        const LogCallback& RO_on_log,
        std::wstring& o_error)
    {
        bool b1_ret = false;
        o_error.clear();

        const std::wstring o_temp_path = MakeTempPath(RO_destination, o_format);
        TempFileCleanup o_cleanup{ o_temp_path };

        if (!ConvertCore(RO_source, o_temp_path, o_format,
            RO_on_progress, RO_on_log, o_error))
        {
            Log(RO_on_log, L"Failed: " + o_error);
            return b1_ret;
        }

        if (!MoveFileExW(o_temp_path.c_str(), RO_destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            o_error = L"Could not move the completed audio file into its destination (Win32 error " +
                std::to_wstring(GetLastError()) + L").";
            Log(RO_on_log, L"Failed: " + o_error);
            return b1_ret;
        }

        Log(RO_on_log, L"Completed successfully: " + RO_destination);

        if (RO_on_progress)
        {
            RO_on_progress(MMGS_PROGRESS_SCALE);
        }

        b1_ret = true;
        return b1_ret;
    }
}

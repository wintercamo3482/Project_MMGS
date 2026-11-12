// File Name:
//   ConverterFrame.cpp
//
// Description:
//   Implements the MFC UI, MP3/WAV selection, Save As, and embedded log.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ConverterFrame.h"
#include "MMGSVersion.h"
#include "MediaConverter.h"

#include <afxdlgs.h>
#include <shlobj.h>

#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <process.h>
#include <string>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#define IDC_SOURCE_BUTTON 1001
#define IDC_SOURCE_EDIT 1002
#define IDC_DEST_BUTTON 1003
#define IDC_DEST_EDIT 1004
#define IDC_START_BUTTON 1005
#define IDC_PROGRESS_BAR 1006
#define IDC_STATUS_TEXT 1007
#define IDC_LOG_TITLE 1008
#define IDC_LOG_EDIT 1009
#define IDC_OPEN_FOLDER_CHECKBOX 1010
#define IDC_FORMAT_LABEL 1011
#define IDC_FORMAT_COMBO 1012

#define WM_CONVERT_PROGRESS (WM_APP + 101)
#define WM_CONVERT_FINISHED (WM_APP + 102)
#define WM_CONVERT_LOG (WM_APP + 103)

#define MMGS_PROGRESS_MAX 100
#define MMGS_LOG_MAX_CHARS (128 * 1024)

namespace
{
    struct ConvertTask
    {
        HWND mo_window = nullptr;
        std::wstring mo_source;
        std::wstring mo_output;
        mmgs::AudioFormat mo_format = mmgs::AudioFormat::Mp3;
    };

    struct ConvertResult
    {
        bool mb1_success = false;
        std::wstring mo_output;
        std::wstring mo_error;
    };

    // Worker threads post log text; MFC controls are updated only by the UI thread.
    void PostLog(HWND o_window, const std::wstring& RO_message)
    {
        auto o_line = std::make_unique<std::wstring>(L"[MMGS] " + RO_message);
        OutputDebugStringW(((*o_line) + L"\r\n").c_str());

        if (PostMessageW(o_window, WM_CONVERT_LOG, 0,
            reinterpret_cast<LPARAM>(o_line.get())))
        {
            o_line.release();
        }
    }

    bool OutputExists(const std::wstring& RO_path)
    {
        bool b1_ret = false;
        const DWORD u32_attributes = GetFileAttributesW(RO_path.c_str());

        if (u32_attributes != INVALID_FILE_ATTRIBUTES &&
            (u32_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            b1_ret = true;
        }

        return b1_ret;
    }

    // Use the Windows Shell to select the newly created audio file in Explorer.
    // AfxOleInit() in MMGSApp.cpp initializes COM on the UI thread.
    bool OpenOutputFolderAndSelectFile(const std::wstring& RO_output_path)
    {
        bool b1_ret = false;
        PIDLIST_ABSOLUTE o_item_id_list = nullptr;
        const HRESULT s32_parse_result = SHParseDisplayName(
            RO_output_path.c_str(), nullptr, &o_item_id_list, 0, nullptr);

        if (SUCCEEDED(s32_parse_result) && o_item_id_list != nullptr)
        {
            const HRESULT s32_open_result = SHOpenFolderAndSelectItems(
                o_item_id_list, 0, nullptr, 0);
            if (SUCCEEDED(s32_open_result))
            {
                b1_ret = true;
            }
        }

        if (o_item_id_list != nullptr)
        {
            CoTaskMemFree(o_item_id_list);
        }

        return b1_ret;
    }

    void SendResult(HWND o_window, std::unique_ptr<ConvertResult> o_result)
    {
        if (PostMessageW(o_window, WM_CONVERT_FINISHED, 0,
            reinterpret_cast<LPARAM>(o_result.get())))
        {
            o_result.release();
        }
    }

    unsigned __stdcall ConvertWorker(void* o_argument)
    {
        uint32_t u32_ret = 0;
        std::unique_ptr<ConvertTask> o_task(static_cast<ConvertTask*>(o_argument));
        auto o_result = std::make_unique<ConvertResult>();
        o_result->mo_output = o_task->mo_output;

        PostLog(o_task->mo_window, L"Source: " + o_task->mo_source);
        PostLog(o_task->mo_window, L"Output: " + o_task->mo_output);

        o_result->mb1_success = mmgs::ConvertMediaToAudio(
            o_task->mo_source,
            o_task->mo_output,
            o_task->mo_format,
            [o_window = o_task->mo_window](int32_t s32_percent)
            {
                PostMessageW(o_window, WM_CONVERT_PROGRESS,
                    static_cast<WPARAM>(s32_percent), 0);
            },
            [o_window = o_task->mo_window](const std::wstring& RO_message)
            {
                PostLog(o_window, RO_message);
            },
            o_result->mo_error);

        SendResult(o_task->mo_window, std::move(o_result));
        return u32_ret;
    }

    bool NormalizeOutputName(const std::wstring& RO_requested_name,
        const std::wstring& RO_expected_extension,
        std::wstring& o_name, std::wstring& o_error)
    {
        bool b1_ret = false;
        o_name.clear();
        o_error.clear();

        if (RO_requested_name.empty())
        {
            o_error = L"Enter an output audio file name.";
            return b1_ret;
        }

        if (RO_requested_name == L"." || RO_requested_name == L".." ||
            RO_requested_name.back() == L' ' ||
            RO_requested_name.back() == L'.')
        {
            o_error = L"A file name cannot end in a space or period.";
            return b1_ret;
        }

        for (const wchar_t a16_character : RO_requested_name)
        {
            if (a16_character < 32 ||
                wcschr(L"<>:\"/\\|?*", a16_character) != nullptr)
            {
                o_error = L"The output file name contains invalid characters.";
                return b1_ret;
            }
        }

        const std::filesystem::path o_requested_path(RO_requested_name);
        const std::wstring o_extension = o_requested_path.extension().wstring();

        if (o_extension.empty())
        {
            o_name = RO_requested_name + RO_expected_extension;
        }
        else if (_wcsicmp(o_extension.c_str(), RO_expected_extension.c_str()) == 0)
        {
            o_name = RO_requested_name;
        }
        else
        {
            o_error = L"The output extension must be " +
                RO_expected_extension + L".";
            return b1_ret;
        }

        const std::wstring o_stem = std::filesystem::path(o_name).stem().wstring();
        const size_t u64_dot = o_stem.find(L'.');
        std::wstring o_device = o_stem.substr(0, u64_dot);

        for (wchar_t& a16_character : o_device)
        {
            a16_character = static_cast<wchar_t>(towupper(a16_character));
        }

        if (o_device == L"CON" || o_device == L"PRN" ||
            o_device == L"AUX" || o_device == L"NUL" ||
            (o_device.size() == 4 &&
                ((o_device.rfind(L"COM", 0) == 0) ||
                    (o_device.rfind(L"LPT", 0) == 0)) &&
                o_device[3] >= L'1' && o_device[3] <= L'9'))
        {
            o_error = L"This is a reserved Windows file name.";
            return b1_ret;
        }

        b1_ret = true;
        return b1_ret;
    }

}

BEGIN_MESSAGE_MAP(CConverterFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_CLOSE()
    ON_BN_CLICKED(IDC_SOURCE_BUTTON, &CConverterFrame::OnSource)
    ON_BN_CLICKED(IDC_DEST_BUTTON, &CConverterFrame::OnDestination)
    ON_CBN_SELCHANGE(IDC_FORMAT_COMBO, &CConverterFrame::OnFormatChanged)
    ON_BN_CLICKED(IDC_START_BUTTON, &CConverterFrame::OnStart)
    ON_MESSAGE(WM_CONVERT_PROGRESS, &CConverterFrame::OnProgress)
    ON_MESSAGE(WM_CONVERT_FINISHED, &CConverterFrame::OnFinished)
    ON_MESSAGE(WM_CONVERT_LOG, &CConverterFrame::OnLog)
END_MESSAGE_MAP()

int CConverterFrame::OnCreate(LPCREATESTRUCT o_create_struct)
{
    int32_t s32_ret = -1;

    if (CFrameWnd::OnCreate(o_create_struct) == -1)
    {
        return s32_ret;
    }

    const DWORD u32_button_style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
    const DWORD u32_edit_style = WS_CHILD | WS_VISIBLE | WS_BORDER |
        ES_AUTOHSCROLL | ES_READONLY;

    mo_source_button.Create(L"Source", u32_button_style,
        CRect(20, 23, 140, 58), this, IDC_SOURCE_BUTTON);
    mo_source_edit.Create(u32_edit_style, CRect(155, 27, 720, 55),
        this, IDC_SOURCE_EDIT);
    mo_dest_button.Create(L"Destination", u32_button_style,
        CRect(20, 79, 140, 114), this, IDC_DEST_BUTTON);
    mo_dest_edit.Create(u32_edit_style, CRect(155, 83, 720, 111),
        this, IDC_DEST_EDIT);
    mo_format_label.Create(L"Output Format", WS_CHILD | WS_VISIBLE | SS_LEFT,
        CRect(20, 137, 140, 161), this, IDC_FORMAT_LABEL);
    mo_format_combo.Create(WS_CHILD | WS_VISIBLE | WS_TABSTOP |
        CBS_DROPDOWNLIST | WS_VSCROLL,
        CRect(155, 132, 310, 260), this, IDC_FORMAT_COMBO);
    mo_format_combo.AddString(L"MP3");
    mo_format_combo.AddString(L"WAV");
    mo_format_combo.SetCurSel(0);
    mo_start_button.Create(L"Start", u32_button_style,
        CRect(20, 185, 140, 223), this, IDC_START_BUTTON);
    mo_progress.Create(WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        CRect(155, 187, 720, 211), this, IDC_PROGRESS_BAR);
    mo_progress.SetRange32(0, MMGS_PROGRESS_MAX);
    mo_progress.SetPos(0);
    mo_status.Create(L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFT,
        CRect(155, 225, 395, 252), this, IDC_STATUS_TEXT);
    mo_open_folder_checkbox.Create(
        L"Open output folder after conversion",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        CRect(400, 222, 720, 253), this, IDC_OPEN_FOLDER_CHECKBOX);
    mo_open_folder_checkbox.SetCheck(BST_CHECKED);
    mo_log_title.Create(L"Debug Log", WS_CHILD | WS_VISIBLE | SS_LEFT,
        CRect(20, 266, 720, 288), this, IDC_LOG_TITLE);
    mo_log_edit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        CRect(20, 291, 720, 555), this, IDC_LOG_EDIT);

    CFont* o_font = CFont::FromHandle(
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));

    mo_source_button.SetFont(o_font);
    mo_source_edit.SetFont(o_font);
    mo_dest_button.SetFont(o_font);
    mo_dest_edit.SetFont(o_font);
    mo_format_label.SetFont(o_font);
    mo_format_combo.SetFont(o_font);
    mo_start_button.SetFont(o_font);
    mo_status.SetFont(o_font);
    mo_open_folder_checkbox.SetFont(o_font);
    mo_log_title.SetFont(o_font);
    mo_log_edit.SetFont(o_font);

    AppendLog(std::wstring(L"[MMGS] Version ") + MMGS_APP_VERSION);
    AppendLog(L"[MMGS] Audio engine: Windows Media Foundation.");
    AppendLog(L"[MMGS] No FFmpeg installation required.");

    s32_ret = 0;
    return s32_ret;
}

void CConverterFrame::AppendLog(const std::wstring& RO_message)
{
    if (mo_log_edit.GetWindowTextLengthW() > MMGS_LOG_MAX_CHARS)
    {
        mo_log_edit.SetWindowTextW(L"[MMGS] Older log entries cleared.\r\n");
    }

    mo_log_edit.SetSel(-1, -1);
    const std::wstring o_line = RO_message + L"\r\n";
    mo_log_edit.ReplaceSel(o_line.c_str(), FALSE);
}

LRESULT CConverterFrame::OnLog(WPARAM, LPARAM o_data)
{
    LRESULT s64_ret = 0;
    std::unique_ptr<std::wstring> o_line(reinterpret_cast<std::wstring*>(o_data));

    if (o_line)
    {
        AppendLog(*o_line);
    }

    return s64_ret;
}

mmgs::AudioFormat CConverterFrame::GetSelectedFormat() const
{
    mmgs::AudioFormat o_ret = mmgs::AudioFormat::Mp3;

    if (mo_format_combo.GetCurSel() == 1)
    {
        o_ret = mmgs::AudioFormat::Wav;
    }

    return o_ret;
}

void CConverterFrame::OnFormatChanged()
{
    // A changed output format requires selecting a matching Save As path.
    mo_dest_path.clear();
    mo_dest_edit.SetWindowTextW(L"");
    mo_status.SetWindowTextW(L"Select a destination file.");
    AppendLog(L"[MMGS] Format changed; select a destination again.");
}

void CConverterFrame::OnSource()
{
    CFileDialog o_dialog(TRUE, L"mp4", nullptr,
        OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
        L"Media files (*.mp4;*.m4a;*.mp3;*.mov;*.wmv;*.wma;*.avi;*.wav)"
        L"|*.mp4;*.m4a;*.mp3;*.mov;*.wmv;*.wma;*.avi;*.wav|"
        L"All files (*.*)|*.*||", this);

    if (o_dialog.DoModal() != IDOK)
    {
        return;
    }

    const std::wstring o_selected_source = o_dialog.GetPathName().GetString();

    if (mo_source_path != o_selected_source)
    {
        // The output must be selected again for a different input file.
        mo_dest_path.clear();
        mo_dest_edit.SetWindowTextW(L"");
    }

    mo_source_path = o_selected_source;
    mo_source_edit.SetWindowTextW(mo_source_path.c_str());
    mo_status.SetWindowTextW(L"Select a destination file.");
}

void CConverterFrame::OnDestination()
{
    const mmgs::AudioFormat o_format = GetSelectedFormat();
    const bool b1_wav = (o_format == mmgs::AudioFormat::Wav);
    const std::wstring o_extension = b1_wav ? L".wav" : L".mp3";
    const wchar_t* RO_filter = b1_wav ?
        L"WAV audio (*.wav)|*.wav||" : L"MP3 audio (*.mp3)|*.mp3||";
    const wchar_t* RO_title = b1_wav ?
        L"Select WAV output file" : L"Select MP3 output file";

    std::wstring o_default_name = L"output" + o_extension;
    std::wstring o_initial_directory = mo_last_dest_directory;

    if (!mo_source_path.empty())
    {
        const std::filesystem::path o_source_path(mo_source_path);
        o_default_name = o_source_path.stem().wstring() + o_extension;

        if (o_initial_directory.empty())
        {
            o_initial_directory = o_source_path.parent_path().wstring();
        }
    }

    if (!mo_dest_path.empty())
    {
        const std::filesystem::path o_previous_path(mo_dest_path);
        o_default_name = o_previous_path.filename().wstring();
        o_initial_directory = o_previous_path.parent_path().wstring();
    }

    // FALSE opens the Save As dialog; the file is created by Start.
    CFileDialog o_dialog(FALSE, o_extension.c_str() + 1,
        o_default_name.c_str(),
        OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR,
        RO_filter, this);
    o_dialog.m_ofn.lpstrTitle = RO_title;

    if (!o_initial_directory.empty())
    {
        o_dialog.m_ofn.lpstrInitialDir = o_initial_directory.c_str();
    }

    if (o_dialog.DoModal() != IDOK)
    {
        return;
    }

    const std::filesystem::path o_selected_path(
        std::wstring(o_dialog.GetPathName().GetString()));
    std::wstring o_output_name;
    std::wstring o_name_error;

    if (!NormalizeOutputName(o_selected_path.filename().wstring(),
        o_extension, o_output_name, o_name_error))
    {
        AfxMessageBox(o_name_error.c_str(), MB_ICONWARNING | MB_OK);
        return;
    }

    const std::filesystem::path o_output_path =
        o_selected_path.parent_path() / o_output_name;
    mo_dest_path = o_output_path.wstring();
    mo_last_dest_directory = o_output_path.parent_path().wstring();
    mo_dest_edit.SetWindowTextW(mo_dest_path.c_str());
    mo_status.SetWindowTextW(L"Ready to convert");
    AppendLog(L"[MMGS] Selected output: " + mo_dest_path);
}

void CConverterFrame::OnStart()
{
    if (mb1_running)
    {
        return;
    }

    if (mo_source_path.empty() || mo_dest_path.empty())
    {
        AfxMessageBox(L"Select a media source and destination audio file first.",
            MB_ICONWARNING | MB_OK);
        return;
    }

    const std::wstring o_output = mo_dest_path;

    if (_wcsicmp(mo_source_path.c_str(), o_output.c_str()) == 0)
    {
        AfxMessageBox(L"The output path equals the source path. Choose a different "
            L"destination audio file.", MB_ICONWARNING | MB_OK);
        return;
    }

    if (OutputExists(o_output))
    {
        const std::wstring o_question =
            L"This output file already exists. Overwrite it?\n\n" + o_output;

        if (AfxMessageBox(o_question.c_str(), MB_ICONQUESTION | MB_YESNO) != IDYES)
        {
            return;
        }
    }

    auto o_task = std::make_unique<ConvertTask>();
    o_task->mo_window = GetSafeHwnd();
    o_task->mo_source = mo_source_path;
    o_task->mo_output = o_output;
    o_task->mo_format = GetSelectedFormat();

    mb1_running = true;
    mo_source_button.EnableWindow(FALSE);
    mo_dest_button.EnableWindow(FALSE);
    mo_format_combo.EnableWindow(FALSE);
    mo_start_button.EnableWindow(FALSE);
    mo_open_folder_checkbox.EnableWindow(FALSE);
    mo_progress.SetPos(0);
    mo_log_edit.SetWindowTextW(L"");
    AppendLog(L"[MMGS] Starting conversion...");
    mo_status.SetWindowTextW(L"Converting... (initializing Media Foundation)");

    const uintptr_t u64_thread = _beginthreadex(
        nullptr, 0, &ConvertWorker, o_task.get(), 0, nullptr);

    if (u64_thread == 0)
    {
        mb1_running = false;
        mo_source_button.EnableWindow(TRUE);
        mo_dest_button.EnableWindow(TRUE);
        mo_format_combo.EnableWindow(TRUE);
        mo_start_button.EnableWindow(TRUE);
        mo_open_folder_checkbox.EnableWindow(TRUE);
        mo_status.SetWindowTextW(L"Failed to start conversion thread");
        AppendLog(L"[MMGS] ERROR: Could not create the conversion thread.");
        AfxMessageBox(L"Could not create the conversion thread.",
            MB_ICONERROR | MB_OK);
        return;
    }

    o_task.release();
    CloseHandle(reinterpret_cast<HANDLE>(u64_thread));
}

LRESULT CConverterFrame::OnProgress(WPARAM u64_percent, LPARAM)
{
    LRESULT s64_ret = 0;

    if (mb1_running)
    {
        const int32_t s32_position = static_cast<int32_t>(u64_percent);
        mo_progress.SetPos(s32_position);
        const std::wstring o_status = L"Converting... " +
            std::to_wstring(s32_position) + L"%";
        mo_status.SetWindowTextW(o_status.c_str());
    }

    return s64_ret;
}

LRESULT CConverterFrame::OnFinished(WPARAM, LPARAM o_data)
{
    LRESULT s64_ret = 0;
    std::unique_ptr<ConvertResult> o_result(
        reinterpret_cast<ConvertResult*>(o_data));

    mb1_running = false;
    mo_source_button.EnableWindow(TRUE);
    mo_dest_button.EnableWindow(TRUE);
    mo_format_combo.EnableWindow(TRUE);
    mo_start_button.EnableWindow(TRUE);
    mo_open_folder_checkbox.EnableWindow(TRUE);

    if (!o_result)
    {
        mo_status.SetWindowTextW(L"Failed - missing conversion result");
        AfxMessageBox(L"The conversion task returned no result.", MB_ICONERROR | MB_OK);
        return s64_ret;
    }

    if (o_result->mb1_success)
    {
        mo_progress.SetPos(MMGS_PROGRESS_MAX);
        mo_status.SetWindowTextW(L"Completed (100%)");
        const std::wstring o_message =
            L"Audio conversion completed.\n\n" + o_result->mo_output;
        AfxMessageBox(o_message.c_str(), MB_ICONINFORMATION | MB_OK);

        if (mo_open_folder_checkbox.GetCheck() == BST_CHECKED)
        {
            if (OpenOutputFolderAndSelectFile(o_result->mo_output))
            {
                AppendLog(L"[MMGS] Opened Explorer and selected the output audio file.");
            }
            else
            {
                AppendLog(L"[MMGS] Warning: Could not open the output folder in Explorer.");
            }
        }
    }
    else
    {
        mo_status.SetWindowTextW(L"Failed - see Debug Log below");
        AfxMessageBox(o_result->mo_error.c_str(), MB_ICONERROR | MB_OK);
    }

    return s64_ret;
}

void CConverterFrame::OnClose()
{
    if (mb1_running)
    {
        AfxMessageBox(L"A conversion is running. Close the app after it finishes.",
            MB_ICONWARNING | MB_OK);
        return;
    }

    CFrameWnd::OnClose();
}

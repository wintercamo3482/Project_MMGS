// File Name:
//   main.cpp
//
// Description:
//   Provides the MMGS MFC interface and embedded conversion log.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <afxwin.h>
#include <afxdlgs.h>
#include <afxcmn.h>
#include <afxole.h>

#include "MMGSVersion.h"
#include "MediaConverter.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <process.h>
#include <string>
#include <utility>

#pragma comment(lib, "comctl32.lib")

#define IDC_SOURCE_BUTTON 1001
#define IDC_SOURCE_EDIT 1002
#define IDC_DEST_BUTTON 1003
#define IDC_DEST_EDIT 1004
#define IDC_START_BUTTON 1005
#define IDC_PROGRESS_BAR 1006
#define IDC_STATUS_TEXT 1007
#define IDC_LOG_TITLE 1008
#define IDC_LOG_EDIT 1009

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

        o_result->mb1_success = mmgs::ConvertMediaToMp3(
            o_task->mo_source,
            o_task->mo_output,
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
}

class CConverterFrame final : public CFrameWnd
{
public:
    CConverterFrame() = default;

protected:
    CButton mo_source_button;
    CEdit mo_source_edit;
    CButton mo_dest_button;
    CEdit mo_dest_edit;
    CButton mo_start_button;
    CProgressCtrl mo_progress;
    CStatic mo_status;
    CStatic mo_log_title;
    CEdit mo_log_edit;

    std::wstring mo_source_path;
    std::wstring mo_dest_folder;
    bool mb1_running = false;

    afx_msg int OnCreate(LPCREATESTRUCT o_create_struct);
    afx_msg void OnSource();
    afx_msg void OnDestination();
    afx_msg void OnStart();
    afx_msg void OnClose();
    afx_msg LRESULT OnProgress(WPARAM u64_percent, LPARAM);
    afx_msg LRESULT OnFinished(WPARAM, LPARAM o_data);
    afx_msg LRESULT OnLog(WPARAM, LPARAM o_data);

    void AppendLog(const std::wstring& RO_message);

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CConverterFrame, CFrameWnd)
    ON_WM_CREATE()
    ON_WM_CLOSE()
    ON_BN_CLICKED(IDC_SOURCE_BUTTON, &CConverterFrame::OnSource)
    ON_BN_CLICKED(IDC_DEST_BUTTON, &CConverterFrame::OnDestination)
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
    mo_start_button.Create(L"Start", u32_button_style,
        CRect(20, 143, 140, 181), this, IDC_START_BUTTON);
    mo_progress.Create(WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        CRect(155, 145, 720, 169), this, IDC_PROGRESS_BAR);
    mo_progress.SetRange32(0, MMGS_PROGRESS_MAX);
    mo_progress.SetPos(0);
    mo_status.Create(L"Ready", WS_CHILD | WS_VISIBLE | SS_LEFT,
        CRect(155, 181, 720, 208), this, IDC_STATUS_TEXT);
    mo_log_title.Create(L"Debug Log", WS_CHILD | WS_VISIBLE | SS_LEFT,
        CRect(20, 220, 720, 242), this, IDC_LOG_TITLE);
    mo_log_edit.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        CRect(20, 245, 720, 495), this, IDC_LOG_EDIT);

    CFont* o_font = CFont::FromHandle(
        static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)));

    mo_source_button.SetFont(o_font);
    mo_source_edit.SetFont(o_font);
    mo_dest_button.SetFont(o_font);
    mo_dest_edit.SetFont(o_font);
    mo_start_button.SetFont(o_font);
    mo_status.SetFont(o_font);
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

void CConverterFrame::OnSource()
{
    CFileDialog o_dialog(TRUE, L"mp4", nullptr,
        OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST,
        L"Media files (*.mp4;*.m4a;*.mov;*.wmv;*.wma;*.avi;*.wav)"
        L"|*.mp4;*.m4a;*.mov;*.wmv;*.wma;*.avi;*.wav|"
        L"All files (*.*)|*.*||", this);

    if (o_dialog.DoModal() != IDOK)
    {
        return;
    }

    mo_source_path = o_dialog.GetPathName().GetString();
    mo_source_edit.SetWindowTextW(mo_source_path.c_str());
}

void CConverterFrame::OnDestination()
{
    CFolderPickerDialog o_dialog(
        mo_dest_folder.empty() ? nullptr : mo_dest_folder.c_str(),
        OFN_PATHMUSTEXIST, this);

    if (o_dialog.DoModal() != IDOK)
    {
        return;
    }

    mo_dest_folder = o_dialog.GetPathName().GetString();
    mo_dest_edit.SetWindowTextW(mo_dest_folder.c_str());
}

void CConverterFrame::OnStart()
{
    if (mb1_running)
    {
        return;
    }

    if (mo_source_path.empty() || mo_dest_folder.empty())
    {
        AfxMessageBox(L"Select a media source and destination folder first.",
            MB_ICONWARNING | MB_OK);
        return;
    }

    const std::filesystem::path o_input_path(mo_source_path);
    const std::filesystem::path o_output_path =
        std::filesystem::path(mo_dest_folder) /
        (o_input_path.stem().wstring() + L".mp3");
    const std::wstring o_output = o_output_path.wstring();

    if (_wcsicmp(mo_source_path.c_str(), o_output.c_str()) == 0)
    {
        AfxMessageBox(L"The output path equals the source path. Choose a different "
            L"destination folder.", MB_ICONWARNING | MB_OK);
        return;
    }

    if (OutputExists(o_output))
    {
        const std::wstring o_question =
            L"This MP3 already exists. Overwrite it?\n\n" + o_output;

        if (AfxMessageBox(o_question.c_str(), MB_ICONQUESTION | MB_YESNO) != IDYES)
        {
            return;
        }
    }

    auto o_task = std::make_unique<ConvertTask>();
    o_task->mo_window = GetSafeHwnd();
    o_task->mo_source = mo_source_path;
    o_task->mo_output = o_output;

    mb1_running = true;
    mo_source_button.EnableWindow(FALSE);
    mo_dest_button.EnableWindow(FALSE);
    mo_start_button.EnableWindow(FALSE);
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
        mo_start_button.EnableWindow(TRUE);
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
    mo_start_button.EnableWindow(TRUE);

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
            L"MP3 conversion completed.\n\n" + o_result->mo_output;
        AfxMessageBox(o_message.c_str(), MB_ICONINFORMATION | MB_OK);
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

class CMMGSApp final : public CWinApp
{
public:
    BOOL InitInstance() override
    {
        BOOL b1_ret = FALSE;
        CWinApp::InitInstance();

        if (!AfxOleInit())
        {
            AfxMessageBox(L"Could not initialize Windows COM/OLE.",
                MB_ICONERROR | MB_OK);
            return b1_ret;
        }

        INITCOMMONCONTROLSEX o_common_controls{};
        o_common_controls.dwSize = sizeof(o_common_controls);
        o_common_controls.dwICC = ICC_PROGRESS_CLASS;
        InitCommonControlsEx(&o_common_controls);

        CConverterFrame* o_window = new CConverterFrame();
        m_pMainWnd = o_window;

        if (!o_window->Create(nullptr, MMGS_WINDOW_TITLE,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CRect(150, 150, 905, 705)))
        {
            m_pMainWnd = nullptr;
            delete o_window;
            return b1_ret;
        }

        o_window->ShowWindow(SW_SHOW);
        o_window->UpdateWindow();
        b1_ret = TRUE;
        return b1_ret;
    }
};

CMMGSApp o_app;
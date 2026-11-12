#pragma once

// File Name:
//   ConverterFrame.h
//
// Description:
//   Declares the MMGS media conversion window and its MFC controls.

#include <afxwin.h>
#include <afxcmn.h>

#include <string>
#include "MediaConverter.h"

class CConverterFrame final : public CFrameWnd
{
public:
    CConverterFrame() = default;

protected:
    CButton mo_source_button;
    CEdit mo_source_edit;
    CButton mo_dest_button;
    CEdit mo_dest_edit;
    CStatic mo_format_label;
    CComboBox mo_format_combo;
    CButton mo_start_button;
    CProgressCtrl mo_progress;
    CStatic mo_status;
    CButton mo_open_folder_checkbox;
    CStatic mo_log_title;
    CEdit mo_log_edit;

    std::wstring mo_source_path;
    std::wstring mo_dest_path;
    std::wstring mo_last_dest_directory;
    bool mb1_running = false;

    afx_msg int OnCreate(LPCREATESTRUCT o_create_struct);
    afx_msg void OnSource();
    afx_msg void OnDestination();
    afx_msg void OnFormatChanged();
    afx_msg void OnStart();
    afx_msg void OnClose();
    afx_msg LRESULT OnProgress(WPARAM u64_percent, LPARAM);
    afx_msg LRESULT OnFinished(WPARAM, LPARAM o_data);
    afx_msg LRESULT OnLog(WPARAM, LPARAM o_data);

    void AppendLog(const std::wstring& RO_message);
    mmgs::AudioFormat GetSelectedFormat() const;

    DECLARE_MESSAGE_MAP()
};

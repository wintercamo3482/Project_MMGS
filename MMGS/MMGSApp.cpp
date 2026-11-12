// File Name:
//   MMGSApp.cpp
//
// Description:
//   Initializes the MFC application and creates its main frame.

#include <afxwin.h>
#include <afxole.h>
#include <commctrl.h>

#include "ConverterFrame.h"
#include "MMGSVersion.h"

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
            CRect(150, 150, 905, 780)))
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

#include "StdAfx.h"
#include "resource.h"
#include "MainDlg.h"
#include "JPEGImage.h"
#include "TagPanel.h"
#include "TagPanelCtl.h"
#include "NavigationPanel.h"
#include "SettingsProvider.h"
#include "TimerEventIDs.h"
#include "EXIFDisplayCtl.h"
#include "ImageProcPanelCtl.h"
#include "RotationPanelCtl.h"
#include "PanelMgr.h"
#include "PaintMemDCMgr.h"
#include "DebugPrint.h"

// Gets the command ID of the file deletion command according to the INI file setting
static int GetDeleteCommandId() {
	Helpers::EDeleteConfirmation confirmation = CSettingsProvider::This().DeleteConfirmation();
	switch (confirmation)
	{
	case Helpers::DC_Never:
		return IDM_MOVE_TO_RECYCLE_BIN;
	case Helpers::DC_OnlyWhenNoRecycleBin:
		return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE;
	default:
		return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM;
	}
}

CTagPanelCtl::CTagPanelCtl(CMainDlg* pMainDlg, CPanel* pImageProcPanel, bool* pFullScreenMode) : CPanelController(pMainDlg, false) {
	m_bEnabled = CSettingsProvider::This().ShowNavPanel();
	m_nMouseX = m_nMouseY = 0;
	m_bMouseInNavPanel = false;
	m_bInNavPanelAnimation = false;
	m_bFadeOut = false;
	m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
	m_nBlendInNavPanelCountdown = 0;
	m_pMemDCAnimation = NULL;
	m_hOffScreenBitmapAnimation = NULL;
	m_pPanel = m_pTagPanel = new CTagPanel(pMainDlg->m_hWnd, this, pImageProcPanel, pMainDlg->GetKeyMap(), pFullScreenMode, &(CMainDlg::IsCurrentImageFitToScreen), pMainDlg);
	m_pTagPanel->GetBtnAddTag()->SetButtonPressedHandler(&OnAddTag, this, 0);
	m_pTagPanel->GetBtnPrev()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_Previous);
	m_pTagPanel->GetBtnNext()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_Next);
	m_pTagPanel->GetBtnEnd()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_Last);
	if (CSettingsProvider::This().AllowFileDeletion()) {
		m_pTagPanel->GetBtnDelete()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, GetDeleteCommandId());
	}
	m_pTagPanel->GetBtnZoomMode()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_ZOOM_MODE, pMainDlg->IsInZoomMode());
	m_pTagPanel->GetBtnFitToScreen()->SetButtonPressedHandler(&OnToggleZoomFit, this);
	m_pTagPanel->GetBtnWindowMode()->SetButtonPressedHandler(&OnToggleWindowMode, this);
	m_pTagPanel->GetBtnRotateCW()->SetButtonPressedHandler(&OnRotate, this, IDM_ROTATE_90);
	m_pTagPanel->GetBtnRotateCCW()->SetButtonPressedHandler(&OnRotate, this, IDM_ROTATE_270);
	m_pTagPanel->GetBtnRotateFree()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_ROTATE);
	m_pTagPanel->GetBtnPerspectiveCorrection()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_PERSPECTIVE);
	m_pTagPanel->GetBtnKeepParams()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_KEEP_PARAMETERS, pMainDlg->IsKeepParams());
	m_pTagPanel->GetBtnLandscapeMode()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_LANDSCAPE_MODE, pMainDlg->IsLandscapeMode());
	m_pTagPanel->GetBtnShowInfo()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_SHOW_FILEINFO, pMainDlg->GetEXIFDisplayCtl()->IsActive());
}

CTagPanelCtl::~CTagPanelCtl() {
	delete m_pTagPanel;
	m_pTagPanel = NULL;
	if (m_pMemDCAnimation != NULL) {
		delete m_pMemDCAnimation;
	}
	if (m_hOffScreenBitmapAnimation != NULL) {
		::DeleteObject(m_hOffScreenBitmapAnimation);
	}
	m_bInNavPanelAnimation = false;
}

void CTagPanelCtl::AdjustMaximalWidth(int nMaxWidth) {
	if (m_pTagPanel->AdjustMaximalWidth(nMaxWidth)) {
		EndNavPanelAnimation();
	}
}

bool CTagPanelCtl::IsVisible() {
	bool bMouseInNavPanel = m_bMouseInNavPanel && !m_pMainDlg->GetImageProcPanelCtl()->IsVisible();
	return m_bEnabled && !(m_fCurrentBlendingFactorNavPanel <= 0.0f && !bMouseInNavPanel) &&
		!m_pMainDlg->IsInMovieMode() && !m_pMainDlg->IsDoCropping() && (m_pMainDlg->IsMouseOn() || bMouseInNavPanel);
}

void CTagPanelCtl::SetActive(bool bActive) {
	if (m_bEnabled != bActive) {
		m_bEnabled = bActive;
		if (m_bEnabled && !m_pMainDlg->GetImageProcPanelCtl()->IsVisible()) {
			EndNavPanelAnimation();
			::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID);
			m_pMainDlg->MouseOn();
		} else {
			m_pTagPanel->GetTooltipMgr().RemoveActiveTooltip();
		}
		InvalidateMainDlg();
	}
}

bool CTagPanelCtl::OnMouseLButton(EMouseEvent eMouseEvent, int nX, int nY) {
	bool bHandled = CPanelController::OnMouseLButton(eMouseEvent, nX, nY);
	if (eMouseEvent == MouseEvent_BtnDown) {
		return CheckMouseInNavPanel(nX, nY) || bHandled;
	}
	return bHandled;
}

bool CTagPanelCtl::OnMouseMove(int nX, int nY) {
	if (!m_bEnabled) {
		return false;
	}

	// Start timer to fade out nav panel if no mouse movement
	bool bModalPanelShown = m_pMainDlg->GetPanelMgr()->IsModalPanelShown();
	bool bImageProcPanelShown = m_pMainDlg->GetImageProcPanelCtl()->IsVisible();
	if ((m_nMouseX != nX || m_nMouseY != nY) && !m_pMainDlg->IsPanMouseCursorSet()) {
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID);
		if (!bImageProcPanelShown && !bModalPanelShown) {
			if (!m_bInNavPanelAnimation) {
				::SetTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID, 2000, NULL);
			} else {
				// Mouse moved - fade in navigation panel
				if (m_pMainDlg->IsMouseOn()) {
					if (m_nBlendInNavPanelCountdown >= 5) {
						StartNavPanelAnimation(false, true);
						m_nBlendInNavPanelCountdown = 0;
					} else {
						m_nBlendInNavPanelCountdown++;
					}
				}
			}
		}
	}

	m_nMouseX = nX;
	m_nMouseY = nY;

	if (bModalPanelShown) {
		SetMouseInNavPanel(false);
	} else {
		if (!bImageProcPanelShown) {
			bool bMouseInNavPanel = PanelRect().PtInRect(CPoint(nX, nY));
			if (!bMouseInNavPanel && m_bMouseInNavPanel) {
				SetMouseInNavPanel(false);
				m_pTagPanel->GetTooltipMgr().EnableTooltips(false);
				m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
			} else if (bMouseInNavPanel && !m_bMouseInNavPanel) {
				StartNavPanelTimer(50);
			}
			return CPanelController::OnMouseMove(nX, nY);
		} else {
			m_pTagPanel->GetTooltipMgr().EnableTooltips(false);
			SetMouseInNavPanel(false);
		}
	}

	return false;
}

bool CTagPanelCtl::OnTimer(int nTimerId) {
	if (nTimerId == TAGPANEL_TIMER_EVENT_ID) {
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_TIMER_EVENT_ID);
		if (m_bEnabled && !m_pMainDlg->GetImageProcPanelCtl()->IsVisible()) {
			bool bMouseInNavPanel = PanelRect().PtInRect(CPoint(m_nMouseX, m_nMouseY));
			if (bMouseInNavPanel && !m_bMouseInNavPanel) {
				SetMouseInNavPanel(true);
				m_pTagPanel->GetTooltipMgr().EnableTooltips(true);
				m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
				EndNavPanelAnimation();
				m_pMainDlg->MouseOn();
			}
		}
		return true;
	} else if (nTimerId == TAGPANEL_START_ANI_TIMER_EVENT_ID) {
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID);
		if (m_bEnabled && !m_bMouseInNavPanel) {
			StartNavPanelAnimation(true, false);
		}
		return true;
	} else if (nTimerId == TAGPANEL_ANI_TIMER_EVENT_ID) {
		DoNavPanelAnimation();
		return true;
	}
	return false;
}

bool CTagPanelCtl::CheckMouseInNavPanel(int nX, int nY) {
	bool bMouseInNavPanel = !m_pMainDlg->GetPanelMgr()->IsModalPanelShown() && PanelRect().PtInRect(CPoint(nX, nY));
	if (bMouseInNavPanel && !m_bMouseInNavPanel) {
		SetMouseInNavPanel(true);
		m_pTagPanel->GetTooltipMgr().EnableTooltips(true);
		m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
	}
	return bMouseInNavPanel;
}

void CTagPanelCtl::StartNavPanelTimer(int nTimeout) {
	::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_TIMER_EVENT_ID);
	::SetTimer(m_pMainDlg->GetHWND(), TAGPANEL_TIMER_EVENT_ID, nTimeout, NULL);
}

void CTagPanelCtl::StartNavPanelAnimation(bool bFadeOut, bool bFast) {
	if (m_pMainDlg->GetPanelMgr()->IsModalPanelShown()) return;
	if (!m_bInNavPanelAnimation) {
		m_bFadeOut = bFadeOut;
		if (!bFadeOut) {
			return; // already visible, do nothing
		}
		m_bInNavPanelAnimation = true;
		m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
		::SetTimer(m_pMainDlg->GetHWND(), TAGPANEL_ANI_TIMER_EVENT_ID, bFast ? 20 : 100, NULL);
	} else if (m_bFadeOut != bFadeOut) {
		m_bFadeOut = bFadeOut;
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_ANI_TIMER_EVENT_ID);
		::SetTimer(m_pMainDlg->GetHWND(), TAGPANEL_ANI_TIMER_EVENT_ID, bFast ? 20 : 100, NULL);
	}
}

void CTagPanelCtl::DoNavPanelAnimation() {
	CJPEGImage* pCurrentImage = CurrentImage();
	bool bDoAnimation = true;
	if (!m_bInNavPanelAnimation || pCurrentImage == NULL) {
		bDoAnimation = false;
	} else {
		if (!IsVisible()) {
			bDoAnimation = false;
		} else if ((m_bFadeOut && m_fCurrentBlendingFactorNavPanel <= 0) || (!m_bFadeOut && m_fCurrentBlendingFactorNavPanel >= CSettingsProvider::This().BlendFactorNavPanel())) {
			bDoAnimation = false;
		}
	}

	bool bTerminate = false;
	if (m_bFadeOut) {
		m_fCurrentBlendingFactorNavPanel = max(0.0f, m_fCurrentBlendingFactorNavPanel - 0.02f);
	} else {
		m_fCurrentBlendingFactorNavPanel = min(CSettingsProvider::This().BlendFactorNavPanel(), m_fCurrentBlendingFactorNavPanel + 0.06f);
		bTerminate = m_fCurrentBlendingFactorNavPanel >= CSettingsProvider::This().BlendFactorNavPanel();
	}

	if (bDoAnimation) {
		CRect rectNavPanel = PanelRect();
		CDC screenDC = m_pMainDlg->GetDC();
		void* pDIBData = pCurrentImage->DIBPixelsLastProcessed(false);
		if (pDIBData != NULL) {
			if (m_pMemDCAnimation == NULL) {
				m_pMemDCAnimation = new CDC();
				m_hOffScreenBitmapAnimation = CPaintMemDCMgr::PrepareRectForMemDCPainting(*m_pMemDCAnimation, screenDC, rectNavPanel);
			}

			CBrush backBrush;
			backBrush.CreateSolidBrush(CSettingsProvider::This().ColorBackground());
			m_pMemDCAnimation->FillRect(CRect(0, 0, rectNavPanel.Width(), rectNavPanel.Height()), backBrush);

			BITMAPINFO bmInfo;
			memset(&bmInfo, 0, sizeof(BITMAPINFO));
			bmInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmInfo.bmiHeader.biWidth = pCurrentImage->DIBWidth();
			bmInfo.bmiHeader.biHeight = -pCurrentImage->DIBHeight();
			bmInfo.bmiHeader.biPlanes = 1;
			bmInfo.bmiHeader.biBitCount = 32;
			bmInfo.bmiHeader.biCompression = BI_RGB;
			int xDest = (m_pMainDlg->ClientRect().Width() - pCurrentImage->DIBWidth()) / 2 + m_pMainDlg->GetDIBOffset().x;
			int yDest = (m_pMainDlg->ClientRect().Height() - pCurrentImage->DIBHeight()) / 2 + m_pMainDlg->GetDIBOffset().y;
			CPaintMemDCMgr::BitBltBlended(*m_pMemDCAnimation, screenDC, CSize(rectNavPanel.Width(), rectNavPanel.Height()), pDIBData, &bmInfo, 
								  CPoint(xDest - rectNavPanel.left, yDest - rectNavPanel.top), CSize(pCurrentImage->DIBWidth(), pCurrentImage->DIBHeight()), 
								  *m_pTagPanel, CPoint(-rectNavPanel.left, -rectNavPanel.top), m_fCurrentBlendingFactorNavPanel);
			screenDC.BitBlt(rectNavPanel.left, rectNavPanel.top, rectNavPanel.Width(), rectNavPanel.Height(), *m_pMemDCAnimation, 0, 0, SRCCOPY);
		}
	}
	if (bTerminate) {
		EndNavPanelAnimation();
	}
}

void CTagPanelCtl::EndNavPanelAnimation() {
	if (m_bInNavPanelAnimation) {
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_ANI_TIMER_EVENT_ID);
		m_bInNavPanelAnimation = false;
		m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
		if (m_pMemDCAnimation != NULL) {
			delete m_pMemDCAnimation;
			m_pMemDCAnimation = NULL;
			::DeleteObject(m_hOffScreenBitmapAnimation);
			m_hOffScreenBitmapAnimation = NULL;
		}
		
	}
	::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID);
	::SetTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID, 2000, NULL);
}

void CTagPanelCtl::HideNavPanelTemporary(bool bForce) {
	if (bForce || !m_bMouseInNavPanel || m_pMainDlg->GetImageProcPanelCtl()->IsVisible() || m_pMainDlg->IsCropping()) {
		m_bInNavPanelAnimation = true;
		m_fCurrentBlendingFactorNavPanel = 0.0;
		m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
		m_bFadeOut = true;
	}
	if (bForce) SetMouseInNavPanel(false);
}

void CTagPanelCtl::ShowNavPanelTemporary() {
	if (!m_pMainDlg->GetImageProcPanelCtl()->IsVisible()) {
		m_bInNavPanelAnimation = false;
		m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
		m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
		m_bFadeOut = true;
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_ANI_TIMER_EVENT_ID);
		::KillTimer(m_pMainDlg->GetHWND(), TAGPANEL_START_ANI_TIMER_EVENT_ID);
	}
}

void CTagPanelCtl::SetMouseInNavPanel(bool value) {
	if (m_bMouseInNavPanel != value) {
		m_bMouseInNavPanel = value;
		ControlsConstIterator iter;
		const std::map<int, CUICtrl*> & controls = m_pTagPanel->GetControls();
		for (iter = controls.begin(); iter != controls.end( ); iter++ ) {
			CButtonCtrl* pButton = dynamic_cast<CButtonCtrl*>(iter->second);
			if (pButton != NULL) {
				pButton->SetDimmingFactor(m_bMouseInNavPanel ? 0.5f : 0.0f);
			}
		}
	}
}

void CTagPanelCtl::MoveMouseCursorToButton(CButtonCtrl & sender, const CRect & oldRect) {
	GetPanel()->RequestRepositioning();
	if (oldRect != GetPanel()->PanelRect()) {
		CRect rect = sender.GetPosition();
		CPoint ptWnd = rect.CenterPoint();
		::ClientToScreen(m_pMainDlg->GetHWND(), &ptWnd);
		::SetCursorPos(ptWnd.x, ptWnd.y);
	}
}


void CTagPanelCtl::OnAddTag(void* pContext, int nParameter, CButtonCtrl& sender) {
	DEBUGPRINT("OnAddTag called.");
}

void CTagPanelCtl::OnGotoImage(void* pContext, int nParameter, CButtonCtrl& sender) {
	CTagPanelCtl* pThis = (CTagPanelCtl*)pContext;
	CRect oldRect = pThis->GetPanel()->PanelRect();
	pThis->m_pMainDlg->GotoImage((CMainDlg::EImagePosition)nParameter);
	pThis->MoveMouseCursorToButton(sender, oldRect);
}

void CTagPanelCtl::OnRotate(void* pContext, int nParameter, CButtonCtrl & sender) {
	CTagPanelCtl* pThis = (CTagPanelCtl*)pContext;
	CRect oldRect = pThis->GetPanel()->PanelRect();
	pThis->m_pMainDlg->ExecuteCommand(nParameter);
	pThis->MoveMouseCursorToButton(sender, oldRect);
}

void CTagPanelCtl::OnToggleZoomFit(void* pContext, int nParameter, CButtonCtrl & sender) {
	CTagPanelCtl* pThis = (CTagPanelCtl*)pContext;
	if (CMainDlg::IsCurrentImageFitToScreen(pThis->m_pMainDlg)) {
		pThis->m_pMainDlg->ResetZoomTo100Percents(pThis->m_pMainDlg->IsMouseOn());
	} else {
		pThis->m_pMainDlg->ResetZoomToFitScreen(false, true, true);
	}
}

void CTagPanelCtl::OnToggleWindowMode(void* pContext, int nParameter, CButtonCtrl & sender) {
	CTagPanelCtl* pThis = (CTagPanelCtl*)pContext;
	CRect oldRect = pThis->GetPanel()->PanelRect();
	pThis->m_pMainDlg->ExecuteCommand(IDM_FULL_SCREEN_MODE);
	pThis->MoveMouseCursorToButton(sender, oldRect);
}

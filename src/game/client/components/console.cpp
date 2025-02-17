/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include <base/logger.h>
#include <base/tl/sorted_array.h>

#include <climits>
#include <cmath>
#include <limits>

#include <game/generated/client_data.h>

#include <base/system.h>

#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/ringbuffer.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <cstdio>
#include <cstring>

#include <game/client/ui.h>

#include <game/version.h>

#include <game/client/components/controls.h>
#include <game/client/components/menus.h>
#include <game/client/lineinput.h>
#include <game/client/render.h>

#include <game/client/gameclient.h>

#include <base/math.h>

#include "console.h"

class CConsoleLogger : public ILogger
{
	CGameConsole *m_pConsole;
	std::mutex m_ConsoleMutex;

public:
	CConsoleLogger(CGameConsole *pConsole) :
		m_pConsole(pConsole)
	{
		dbg_assert(pConsole != nullptr, "console pointer must not be null");
	}

	void Log(const CLogMessage *pMessage) override;
	void OnConsoleDeletion();
};

void CConsoleLogger::Log(const CLogMessage *pMessage)
{
	// TODO: Fix thread-unsafety of accessing `g_Config.m_ConsoleOutputLevel`
	if(pMessage->m_Level > IConsole::ToLogLevel(g_Config.m_ConsoleOutputLevel))
	{
		return;
	}
	ColorRGBA Color = gs_ConsoleDefaultColor;
	if(pMessage->m_HaveColor)
	{
		Color.r = pMessage->m_Color.r / 255.0;
		Color.g = pMessage->m_Color.g / 255.0;
		Color.b = pMessage->m_Color.b / 255.0;
	}
	std::unique_lock<std::mutex> Guard(m_ConsoleMutex);
	if(m_pConsole)
	{
		m_pConsole->m_LocalConsole.PrintLine(pMessage->m_aLine, pMessage->m_LineLength, Color);
	}
}

void CConsoleLogger::OnConsoleDeletion()
{
	std::unique_lock<std::mutex> Guard(m_ConsoleMutex);
	m_pConsole = nullptr;
}

CGameConsole::CInstance::CInstance(int Type)
{
	m_pHistoryEntry = 0x0;

	m_Type = Type;

	if(Type == CGameConsole::CONSOLETYPE_LOCAL)
		m_CompletionFlagmask = CFGFLAG_CLIENT;
	else
		m_CompletionFlagmask = CFGFLAG_SERVER;

	m_aCompletionBuffer[0] = 0;
	m_CompletionUsed = false;
	m_CompletionChosen = -1;
	m_CompletionRenderOffset = 0.0f;
	m_ReverseTAB = false;

	m_aUser[0] = '\0';
	m_UserGot = false;
	m_UsernameReq = false;

	m_IsCommand = false;
}

void CGameConsole::CInstance::Init(CGameConsole *pGameConsole)
{
	m_pGameConsole = pGameConsole;
}

void CGameConsole::CInstance::ClearBacklog()
{
	m_BacklogLock.lock();
	m_Backlog.Init();
	m_BacklogCurPage = 0;
	m_BacklogLock.unlock();
}

void CGameConsole::CInstance::ClearBacklogYOffsets()
{
	m_BacklogLock.lock();
	auto *pEntry = m_Backlog.First();
	while(pEntry)
	{
		pEntry->m_YOffset = -1.0f;
		pEntry = m_Backlog.Next(pEntry);
	}
	m_BacklogLock.unlock();
}

void CGameConsole::CInstance::ClearHistory()
{
	m_History.Init();
	m_pHistoryEntry = 0;
}

void CGameConsole::CInstance::ExecuteLine(const char *pLine)
{
	if(m_Type == CGameConsole::CONSOLETYPE_LOCAL)
		m_pGameConsole->m_pConsole->ExecuteLine(pLine);
	else
	{
		if(m_pGameConsole->Client()->RconAuthed())
			m_pGameConsole->Client()->Rcon(pLine);
		else
		{
			if(!m_UserGot && m_UsernameReq)
			{
				m_UserGot = true;
				str_copy(m_aUser, pLine, sizeof m_aUser);
			}
			else
			{
				m_pGameConsole->Client()->RconAuth(m_aUser, pLine);
				m_UserGot = false;
			}
		}
	}
}

void CGameConsole::CInstance::PossibleCommandsCompleteCallback(const char *pStr, void *pUser)
{
	CGameConsole::CInstance *pInstance = (CGameConsole::CInstance *)pUser;
	if(pInstance->m_CompletionChosen == pInstance->m_CompletionEnumerationCount)
		pInstance->m_Input.Set(pStr);
	pInstance->m_CompletionEnumerationCount++;
}

void CGameConsole::CInstance::OnInput(IInput::CEvent Event)
{
	bool Handled = false;

	if(m_pGameConsole->Input()->ModifierIsPressed()) // jump to spaces and special ASCII characters
	{
		int SearchDirection = 0;
		if(m_pGameConsole->Input()->KeyPress(KEY_LEFT) || m_pGameConsole->Input()->KeyPress(KEY_BACKSPACE))
			SearchDirection = -1;
		else if(m_pGameConsole->Input()->KeyPress(KEY_RIGHT) || m_pGameConsole->Input()->KeyPress(KEY_DELETE))
			SearchDirection = 1;

		if(SearchDirection != 0)
		{
			int OldOffset = m_Input.GetCursorOffset();

			int FoundAt = SearchDirection > 0 ? m_Input.GetLength() - 1 : 0;
			for(int i = m_Input.GetCursorOffset() + SearchDirection; SearchDirection > 0 ? i < m_Input.GetLength() - 1 : i > 0; i += SearchDirection)
			{
				int Next = i + SearchDirection;
				if((m_Input.GetString()[Next] == ' ') ||
					(m_Input.GetString()[Next] >= 32 && m_Input.GetString()[Next] <= 47) ||
					(m_Input.GetString()[Next] >= 58 && m_Input.GetString()[Next] <= 64) ||
					(m_Input.GetString()[Next] >= 91 && m_Input.GetString()[Next] <= 96))
				{
					FoundAt = i;
					if(SearchDirection < 0)
						FoundAt++;
					break;
				}
			}

			if(m_pGameConsole->Input()->KeyPress(KEY_BACKSPACE))
			{
				if(m_Input.GetCursorOffset() != 0)
				{
					char aText[512];
					str_copy(aText, m_Input.GetString(), FoundAt + 1);

					if(m_Input.GetCursorOffset() != str_length(m_Input.GetString()))
						str_append(aText, m_Input.GetString() + m_Input.GetCursorOffset(), str_length(m_Input.GetString()));

					m_Input.Set(aText);
				}
			}

			if(m_pGameConsole->Input()->KeyPress(KEY_DELETE))
			{
				if(m_Input.GetCursorOffset() != m_Input.GetLength())
				{
					char aText[512];
					aText[0] = '\0';

					str_copy(aText, m_Input.GetString(), m_Input.GetCursorOffset() + 1);

					if(FoundAt != m_Input.GetLength())
						str_append(aText, m_Input.GetString() + FoundAt, sizeof(aText));

					m_Input.Set(aText);
					FoundAt = OldOffset;
				}
			}
			m_Input.SetCursorOffset(FoundAt);
		}
	}
	if(m_pGameConsole->Input()->ModifierIsPressed() && m_pGameConsole->Input()->KeyPress(KEY_V))
	{
		const char *Text = m_pGameConsole->Input()->GetClipboardText();
		if(Text)
		{
			char aLine[256];
			int i, Begin = 0;
			for(i = 0; i < str_length(Text); i++)
			{
				if(Text[i] == '\n')
				{
					if(i == Begin)
					{
						Begin++;
						continue;
					}
					int max = minimum(i - Begin + 1, (int)sizeof(aLine));
					str_copy(aLine, Text + Begin, max);
					Begin = i + 1;
					ExecuteLine(aLine);
				}
			}
			int max = minimum(i - Begin + 1, (int)sizeof(aLine));
			str_copy(aLine, Text + Begin, max);
			m_Input.Append(aLine);
		}
	}
	else if(m_pGameConsole->Input()->ModifierIsPressed() && m_pGameConsole->Input()->KeyPress(KEY_C))
	{
		m_pGameConsole->Input()->SetClipboardText(m_Input.GetString());
	}
	else if(m_pGameConsole->Input()->ModifierIsPressed() && m_pGameConsole->Input()->KeyPress(KEY_A))
	{
		m_Input.SetCursorOffset(0);
	}
	else if(m_pGameConsole->Input()->ModifierIsPressed() && m_pGameConsole->Input()->KeyPress(KEY_E))
	{
		m_Input.SetCursorOffset(m_Input.GetLength());
	}
	else if(m_pGameConsole->Input()->ModifierIsPressed() && m_pGameConsole->Input()->KeyPress(KEY_U))
	{
		m_Input.SetRange("", 0, m_Input.GetCursorOffset());
	}
	else if(m_pGameConsole->Input()->ModifierIsPressed() && m_pGameConsole->Input()->KeyPress(KEY_K))
	{
		m_Input.SetRange("", m_Input.GetCursorOffset(), m_Input.GetLength());
	}

	if(Event.m_Flags & IInput::FLAG_PRESS)
	{
		if(Event.m_Key == KEY_RETURN || Event.m_Key == KEY_KP_ENTER)
		{
			if(m_Input.GetString()[0] || (m_UsernameReq && !m_pGameConsole->Client()->RconAuthed() && !m_UserGot))
			{
				if(m_Type == CONSOLETYPE_LOCAL || m_pGameConsole->Client()->RconAuthed())
				{
					char *pEntry = m_History.Allocate(m_Input.GetLength() + 1);
					mem_copy(pEntry, m_Input.GetString(), m_Input.GetLength() + 1);
				}
				ExecuteLine(m_Input.GetString());
				m_Input.Clear();
				m_pHistoryEntry = 0x0;
			}

			Handled = true;
		}
		else if(Event.m_Key == KEY_UP)
		{
			if(m_pHistoryEntry)
			{
				char *pTest = m_History.Prev(m_pHistoryEntry);

				if(pTest)
					m_pHistoryEntry = pTest;
			}
			else
				m_pHistoryEntry = m_History.Last();

			if(m_pHistoryEntry)
				m_Input.Set(m_pHistoryEntry);
			Handled = true;
		}
		else if(Event.m_Key == KEY_DOWN)
		{
			if(m_pHistoryEntry)
				m_pHistoryEntry = m_History.Next(m_pHistoryEntry);

			if(m_pHistoryEntry)
				m_Input.Set(m_pHistoryEntry);
			else
				m_Input.Clear();
			Handled = true;
		}
		else if(Event.m_Key == KEY_TAB)
		{
			if(m_Type == CGameConsole::CONSOLETYPE_LOCAL || m_pGameConsole->Client()->RconAuthed())
			{
				if(m_ReverseTAB && m_CompletionUsed)
					m_CompletionChosen--;
				else if(!m_ReverseTAB)
					m_CompletionChosen++;
				m_CompletionEnumerationCount = 0;
				m_pGameConsole->m_pConsole->PossibleCommands(m_aCompletionBuffer, m_CompletionFlagmask, m_Type != CGameConsole::CONSOLETYPE_LOCAL && m_pGameConsole->Client()->RconAuthed() && m_pGameConsole->Client()->UseTempRconCommands(), PossibleCommandsCompleteCallback, this);

				m_CompletionUsed = true;

				// handle wrapping
				if(m_CompletionEnumerationCount && (m_CompletionChosen >= m_CompletionEnumerationCount || m_CompletionChosen < 0))
				{
					m_CompletionChosen = (m_CompletionChosen + m_CompletionEnumerationCount) % m_CompletionEnumerationCount;
					m_CompletionEnumerationCount = 0;
					m_pGameConsole->m_pConsole->PossibleCommands(m_aCompletionBuffer, m_CompletionFlagmask, m_Type != CGameConsole::CONSOLETYPE_LOCAL && m_pGameConsole->Client()->RconAuthed() && m_pGameConsole->Client()->UseTempRconCommands(), PossibleCommandsCompleteCallback, this);
				}
			}
		}
		else if(Event.m_Key == KEY_PAGEUP)
		{
			++m_BacklogCurPage;
			m_pGameConsole->m_HasSelection = false;
		}
		else if(Event.m_Key == KEY_PAGEDOWN)
		{
			m_pGameConsole->m_HasSelection = false;
			--m_BacklogCurPage;
			if(m_BacklogCurPage < 0)
				m_BacklogCurPage = 0;
		}
		// in order not to conflict with CLineInput's handling of Home/End only
		// react to it when the input is empty
		else if(Event.m_Key == KEY_HOME && m_Input.GetString()[0] == '\0')
		{
			m_BacklogCurPage = INT_MAX;
			m_pGameConsole->m_HasSelection = false;
		}
		else if(Event.m_Key == KEY_END && m_Input.GetString()[0] == '\0')
		{
			m_BacklogCurPage = 0;
			m_pGameConsole->m_HasSelection = false;
		}
		else if(Event.m_Key == KEY_LSHIFT)
		{
			m_ReverseTAB = true;
			Handled = true;
		}
	}
	if(Event.m_Flags & IInput::FLAG_RELEASE && Event.m_Key == KEY_LSHIFT)
	{
		m_ReverseTAB = false;
		Handled = true;
	}

	if(!Handled)
		m_Input.ProcessInput(Event);

	if(Event.m_Flags & (IInput::FLAG_PRESS | IInput::FLAG_TEXT))
	{
		if((Event.m_Key != KEY_TAB) && (Event.m_Key != KEY_LSHIFT))
		{
			m_CompletionUsed = false;
			m_CompletionChosen = -1;
			str_copy(m_aCompletionBuffer, m_Input.GetString(), sizeof(m_aCompletionBuffer));
			m_CompletionRenderOffset = 0.0f;
		}

		// find the current command
		{
			char aBuf[64] = {0};
			const char *pSrc = GetString();
			int i = 0;
			for(; i < (int)sizeof(aBuf) - 1 && *pSrc && *pSrc != ' '; i++, pSrc++)
				aBuf[i] = *pSrc;
			aBuf[i] = 0;

			const IConsole::CCommandInfo *pCommand = m_pGameConsole->m_pConsole->GetCommandInfo(aBuf, m_CompletionFlagmask,
				m_Type != CGameConsole::CONSOLETYPE_LOCAL && m_pGameConsole->Client()->RconAuthed() && m_pGameConsole->Client()->UseTempRconCommands());
			if(pCommand)
			{
				m_IsCommand = true;
				str_copy(m_aCommandName, pCommand->m_pName, IConsole::TEMPCMD_NAME_LENGTH);
				str_copy(m_aCommandHelp, pCommand->m_pHelp, IConsole::TEMPCMD_HELP_LENGTH);
				str_copy(m_aCommandParams, pCommand->m_pParams, IConsole::TEMPCMD_PARAMS_LENGTH);
			}
			else
				m_IsCommand = false;
		}
	}
}

void CGameConsole::CInstance::PrintLine(const char *pLine, int Len, ColorRGBA PrintColor)
{
	if(Len > 255)
		Len = 255;

	m_BacklogLock.lock();
	CBacklogEntry *pEntry = m_Backlog.Allocate(sizeof(CBacklogEntry) + Len);
	pEntry->m_YOffset = -1.0f;
	pEntry->m_PrintColor = PrintColor;
	mem_copy(pEntry->m_aText, pLine, Len);
	pEntry->m_aText[Len] = 0;
	if(m_pGameConsole->m_ConsoleType == m_Type)
		m_pGameConsole->m_NewLineCounter++;
	m_BacklogLock.unlock();
}

CGameConsole::CGameConsole() :
	m_LocalConsole(CONSOLETYPE_LOCAL), m_RemoteConsole(CONSOLETYPE_REMOTE)
{
	m_ConsoleType = CONSOLETYPE_LOCAL;
	m_ConsoleState = CONSOLE_CLOSED;
	m_StateChangeEnd = 0.0f;
	m_StateChangeDuration = 0.1f;
}

CGameConsole::~CGameConsole()
{
	m_pConsoleLogger->OnConsoleDeletion();
}

float CGameConsole::TimeNow()
{
	static long long s_TimeStart = time_get();
	return float(time_get() - s_TimeStart) / float(time_freq());
}

CGameConsole::CInstance *CGameConsole::CurrentConsole()
{
	if(m_ConsoleType == CONSOLETYPE_REMOTE)
		return &m_RemoteConsole;
	return &m_LocalConsole;
}

void CGameConsole::OnReset()
{
}

// only defined for 0<=t<=1
static float ConsoleScaleFunc(float t)
{
	//return t;
	return sinf(acosf(1.0f - t));
}

struct CRenderInfo
{
	CGameConsole *m_pSelf;
	CTextCursor m_Cursor;
	const char *m_pCurrentCmd;
	int m_WantedCompletion;
	int m_EnumCount;
	float m_Offset;
	float m_Width;
};

void CGameConsole::PossibleCommandsRenderCallback(const char *pStr, void *pUser)
{
	CRenderInfo *pInfo = static_cast<CRenderInfo *>(pUser);

	if(pInfo->m_EnumCount == pInfo->m_WantedCompletion)
	{
		float tw = pInfo->m_pSelf->TextRender()->TextWidth(pInfo->m_Cursor.m_pFont, pInfo->m_Cursor.m_FontSize, pStr, -1, -1.0f);
		pInfo->m_pSelf->Graphics()->TextureClear();
		pInfo->m_pSelf->Graphics()->QuadsBegin();
		pInfo->m_pSelf->Graphics()->SetColor(229.0f / 255.0f, 185.0f / 255.0f, 4.0f / 255.0f, 0.85f);
		pInfo->m_pSelf->RenderTools()->DrawRoundRect(pInfo->m_Cursor.m_X - 2.5f, pInfo->m_Cursor.m_Y - 4.f / 2.f, tw + 5.f, pInfo->m_Cursor.m_FontSize + 4.f, pInfo->m_Cursor.m_FontSize / 3.f);
		pInfo->m_pSelf->Graphics()->QuadsEnd();

		// scroll when out of sight
		if(pInfo->m_Cursor.m_X < 3.0f)
			pInfo->m_Offset = 0.0f;
		else if(pInfo->m_Cursor.m_X + tw > pInfo->m_Width)
			pInfo->m_Offset -= pInfo->m_Width / 2;

		pInfo->m_pSelf->TextRender()->TextColor(0.05f, 0.05f, 0.05f, 1);
		pInfo->m_pSelf->TextRender()->TextEx(&pInfo->m_Cursor, pStr, -1);
	}
	else
	{
		const char *pMatchStart = str_find_nocase(pStr, pInfo->m_pCurrentCmd);

		if(pMatchStart)
		{
			pInfo->m_pSelf->TextRender()->TextColor(0.5f, 0.5f, 0.5f, 1);
			pInfo->m_pSelf->TextRender()->TextEx(&pInfo->m_Cursor, pStr, pMatchStart - pStr);
			pInfo->m_pSelf->TextRender()->TextColor(229.0f / 255.0f, 185.0f / 255.0f, 4.0f / 255.0f, 1);
			pInfo->m_pSelf->TextRender()->TextEx(&pInfo->m_Cursor, pMatchStart, str_length(pInfo->m_pCurrentCmd));
			pInfo->m_pSelf->TextRender()->TextColor(0.5f, 0.5f, 0.5f, 1);
			pInfo->m_pSelf->TextRender()->TextEx(&pInfo->m_Cursor, pMatchStart + str_length(pInfo->m_pCurrentCmd), -1);
		}
		else
		{
			pInfo->m_pSelf->TextRender()->TextColor(0.75f, 0.75f, 0.75f, 1);
			pInfo->m_pSelf->TextRender()->TextEx(&pInfo->m_Cursor, pStr, -1);
		}
	}

	pInfo->m_EnumCount++;
	pInfo->m_Cursor.m_X += 7.0f;
}

void CGameConsole::OnRender()
{
	CUIRect Screen = *UI()->Screen();
	float ConsoleMaxHeight = Screen.h * 3 / 5.0f;
	float ConsoleHeight;

	float Progress = (TimeNow() - (m_StateChangeEnd - m_StateChangeDuration)) / m_StateChangeDuration;

	if(Progress >= 1.0f)
	{
		if(m_ConsoleState == CONSOLE_CLOSING)
			m_ConsoleState = CONSOLE_CLOSED;
		else if(m_ConsoleState == CONSOLE_OPENING)
			m_ConsoleState = CONSOLE_OPEN;

		Progress = 1.0f;
	}

	if(m_ConsoleState == CONSOLE_OPEN && g_Config.m_ClEditor)
		Toggle(CONSOLETYPE_LOCAL);

	if(m_ConsoleState == CONSOLE_CLOSED)
		return;

	if(m_ConsoleState == CONSOLE_OPEN)
		Input()->MouseModeAbsolute();

	float ConsoleHeightScale;

	if(m_ConsoleState == CONSOLE_OPENING)
		ConsoleHeightScale = ConsoleScaleFunc(Progress);
	else if(m_ConsoleState == CONSOLE_CLOSING)
		ConsoleHeightScale = ConsoleScaleFunc(1.0f - Progress);
	else //if (console_state == CONSOLE_OPEN)
		ConsoleHeightScale = ConsoleScaleFunc(1.0f);

	ConsoleHeight = ConsoleHeightScale * ConsoleMaxHeight;

	UI()->MapScreen();

	// do console shadow
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	IGraphics::CColorVertex Array[4] = {
		IGraphics::CColorVertex(0, 0, 0, 0, 0.5f),
		IGraphics::CColorVertex(1, 0, 0, 0, 0.5f),
		IGraphics::CColorVertex(2, 0, 0, 0, 0.0f),
		IGraphics::CColorVertex(3, 0, 0, 0, 0.0f)};
	Graphics()->SetColorVertex(Array, 4);
	IGraphics::CQuadItem QuadItem(0, ConsoleHeight, Screen.w, 10.0f);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();

	// do background
	Graphics()->TextureSet(g_pData->m_aImages[IMAGE_CONSOLE_BG].m_Id);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0.2f, 0.2f, 0.2f, 0.9f);
	if(m_ConsoleType == CONSOLETYPE_REMOTE)
		Graphics()->SetColor(0.4f, 0.2f, 0.2f, 0.9f);
	Graphics()->QuadsSetSubset(0, -ConsoleHeight * 0.075f, Screen.w * 0.075f * 0.5f, 0);
	QuadItem = IGraphics::CQuadItem(0, 0, Screen.w, ConsoleHeight);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();

	// do small bar shadow
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Array[0] = IGraphics::CColorVertex(0, 0, 0, 0, 0.0f);
	Array[1] = IGraphics::CColorVertex(1, 0, 0, 0, 0.0f);
	Array[2] = IGraphics::CColorVertex(2, 0, 0, 0, 0.25f);
	Array[3] = IGraphics::CColorVertex(3, 0, 0, 0, 0.25f);
	Graphics()->SetColorVertex(Array, 4);
	QuadItem = IGraphics::CQuadItem(0, ConsoleHeight - 20, Screen.w, 10);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();

	// do the lower bar
	Graphics()->TextureSet(g_pData->m_aImages[IMAGE_CONSOLE_BAR].m_Id);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.9f);
	Graphics()->QuadsSetSubset(0, 0.1f, Screen.w * 0.015f, 1 - 0.1f);
	QuadItem = IGraphics::CQuadItem(0, ConsoleHeight - 10.0f, Screen.w, 10.0f);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	Graphics()->QuadsEnd();

	ConsoleHeight -= 22.0f;

	CInstance *pConsole = CurrentConsole();

	{
		float FontSize = 10.0f;
		float RowHeight = FontSize * 1.25f;
		float x = 3;
		float y = ConsoleHeight - RowHeight - 5.0f;

		CRenderInfo Info;
		Info.m_pSelf = this;
		Info.m_WantedCompletion = pConsole->m_CompletionUsed ? pConsole->m_CompletionChosen : -1;
		Info.m_EnumCount = 0;
		Info.m_Offset = pConsole->m_CompletionRenderOffset;
		Info.m_Width = Screen.w;
		Info.m_pCurrentCmd = pConsole->m_aCompletionBuffer;
		TextRender()->SetCursor(&Info.m_Cursor, x + Info.m_Offset, y + RowHeight + 2.0f, FontSize, TEXTFLAG_RENDER | TEXTFLAG_STOP_AT_END);
		Info.m_Cursor.m_LineWidth = std::numeric_limits<float>::max();

		// render prompt
		CTextCursor Cursor;
		TextRender()->SetCursor(&Cursor, x, y, FontSize, TEXTFLAG_RENDER);
		const char *pPrompt = "> ";
		if(m_ConsoleType == CONSOLETYPE_REMOTE)
		{
			if(Client()->State() == IClient::STATE_LOADING || Client()->State() == IClient::STATE_ONLINE)
			{
				if(Client()->RconAuthed())
					pPrompt = "rcon> ";
				else
				{
					if(pConsole->m_UsernameReq)
					{
						if(!pConsole->m_UserGot)
							pPrompt = "Enter Username> ";
						else
							pPrompt = "Enter Password> ";
					}
					else
						pPrompt = "Enter Password> ";
				}
			}
			else
				pPrompt = "NOT CONNECTED> ";
		}
		TextRender()->TextEx(&Cursor, pPrompt, -1);

		x = Cursor.m_X;

		//console text editing
		bool Editing = false;
		int EditingCursor = Input()->GetEditingCursor();
		if(Input()->GetIMEState())
		{
			if(str_length(Input()->GetIMEEditingText()))
			{
				pConsole->m_Input.Editing(Input()->GetIMEEditingText(), EditingCursor);
				Editing = true;
			}
		}

		//hide rcon password
		char aInputString[512];
		str_copy(aInputString, pConsole->m_Input.GetString(Editing), sizeof(aInputString));
		if(m_ConsoleType == CONSOLETYPE_REMOTE && Client()->State() == IClient::STATE_ONLINE && !Client()->RconAuthed() && (pConsole->m_UserGot || !pConsole->m_UsernameReq))
		{
			for(int i = 0; i < pConsole->m_Input.GetLength(Editing); ++i)
				aInputString[i] = '*';
		}

		// render console input (wrap line)
		TextRender()->SetCursor(&Cursor, x, y, FontSize, 0);
		Cursor.m_LineWidth = Screen.w - 10.0f - x;
		TextRender()->TextEx(&Cursor, aInputString, pConsole->m_Input.GetCursorOffset(Editing));
		TextRender()->TextEx(&Cursor, aInputString + pConsole->m_Input.GetCursorOffset(Editing), -1);
		int Lines = Cursor.m_LineCount;

		int InputExtraLineCount = Lines - 1;
		y -= InputExtraLineCount * FontSize;
		TextRender()->SetCursor(&Cursor, x, y, FontSize, TEXTFLAG_RENDER);
		Cursor.m_LineWidth = Screen.w - 10.0f - x;

		if(m_LastInputLineCount != InputExtraLineCount)
		{
			m_HasSelection = false;
			m_MouseIsPress = false;
			m_LastInputLineCount = InputExtraLineCount;
		}

		TextRender()->TextEx(&Cursor, aInputString, pConsole->m_Input.GetCursorOffset(Editing));
		CTextCursor Marker = Cursor;
		Marker.m_LineWidth = -1;
		TextRender()->TextEx(&Marker, "|", -1);
		TextRender()->TextEx(&Cursor, aInputString + pConsole->m_Input.GetCursorOffset(Editing), -1);
		Input()->SetEditingPosition(Marker.m_X, Marker.m_Y + Marker.m_FontSize);

		// render possible commands
		if(m_ConsoleType == CONSOLETYPE_LOCAL || Client()->RconAuthed())
		{
			if(pConsole->m_Input.GetString()[0] != 0)
			{
				m_pConsole->PossibleCommands(pConsole->m_aCompletionBuffer, pConsole->m_CompletionFlagmask, m_ConsoleType != CGameConsole::CONSOLETYPE_LOCAL && Client()->RconAuthed() && Client()->UseTempRconCommands(), PossibleCommandsRenderCallback, &Info);
				pConsole->m_CompletionRenderOffset = Info.m_Offset;

				if(Info.m_EnumCount <= 0)
				{
					if(pConsole->m_IsCommand)
					{
						char aBuf[512];
						str_format(aBuf, sizeof(aBuf), "Help: %s ", pConsole->m_aCommandHelp);
						TextRender()->TextEx(&Info.m_Cursor, aBuf, -1);
						TextRender()->TextColor(0.75f, 0.75f, 0.75f, 1);
						str_format(aBuf, sizeof(aBuf), "Usage: %s %s", pConsole->m_aCommandName, pConsole->m_aCommandParams);
						TextRender()->TextEx(&Info.m_Cursor, aBuf, -1);
					}
				}
			}
		}

		pConsole->m_BacklogLock.lock();

		// render log (current page, wrap lines)
		CInstance::CBacklogEntry *pEntry = pConsole->m_Backlog.Last();
		float OffsetY = 0.0f;
		float LineOffset = 1.0f;

		bool WantsSelectionCopy = false;
		if(Input()->ModifierIsPressed() && Input()->KeyPress(KEY_C))
			WantsSelectionCopy = true;
		std::string SelectionString;

		// check if mouse is pressed
		if(!m_MouseIsPress && Input()->NativeMousePressed(1))
		{
			m_MouseIsPress = true;
			Input()->NativeMousePos(&m_MousePressX, &m_MousePressY);
			m_MousePressX = (m_MousePressX / (float)Graphics()->WindowWidth()) * Screen.w;
			m_MousePressY = (m_MousePressY / (float)Graphics()->WindowHeight()) * Screen.h;
		}
		if(m_MouseIsPress)
		{
			Input()->NativeMousePos(&m_MouseCurX, &m_MouseCurY);
			m_MouseCurX = (m_MouseCurX / (float)Graphics()->WindowWidth()) * Screen.w;
			m_MouseCurY = (m_MouseCurY / (float)Graphics()->WindowHeight()) * Screen.h;
		}
		if(m_MouseIsPress && !Input()->NativeMousePressed(1))
		{
			m_MouseIsPress = false;
		}

		static int s_LastActivePage = pConsole->m_BacklogCurPage;
		int TotalPages = 1;
		for(int Page = 0; Page <= s_LastActivePage; ++Page, OffsetY = 0.0f)
		{
			while(pEntry)
			{
				TextRender()->TextColor(pEntry->m_PrintColor);

				// get y offset (calculate it if we haven't yet)
				if(pEntry->m_YOffset < 0.0f)
				{
					TextRender()->SetCursor(&Cursor, 0.0f, 0.0f, FontSize, 0);
					Cursor.m_LineWidth = Screen.w - 10;
					TextRender()->TextEx(&Cursor, pEntry->m_aText, -1);
					pEntry->m_YOffset = Cursor.m_Y + Cursor.m_AlignedFontSize + LineOffset;
				}
				OffsetY += pEntry->m_YOffset;

				if((m_HasSelection || m_MouseIsPress) && m_NewLineCounter > 0)
				{
					float MouseExtraOff = pEntry->m_YOffset;
					m_MousePressY -= MouseExtraOff;
					if(!m_MouseIsPress)
						m_MouseCurY -= MouseExtraOff;
				}

				// next page when lines reach the top
				if(y - OffsetY <= RowHeight)
					break;

				// just render output from current backlog page (render bottom up)
				if(Page == s_LastActivePage)
				{
					TextRender()->SetCursor(&Cursor, 0.0f, y - OffsetY, FontSize, TEXTFLAG_RENDER);
					Cursor.m_LineWidth = Screen.w - 10.0f;
					Cursor.m_CalculateSelectionMode = (m_MouseIsPress || (m_CurSelStart != m_CurSelEnd) || m_HasSelection) ? TEXT_CURSOR_SELECTION_MODE_CALCULATE : TEXT_CURSOR_SELECTION_MODE_NONE;
					Cursor.m_PressMouseX = m_MousePressX;
					Cursor.m_PressMouseY = m_MousePressY;
					Cursor.m_ReleaseMouseX = m_MouseCurX;
					Cursor.m_ReleaseMouseY = m_MouseCurY;
					TextRender()->TextEx(&Cursor, pEntry->m_aText, -1);
					if(Cursor.m_CalculateSelectionMode == TEXT_CURSOR_SELECTION_MODE_CALCULATE)
					{
						m_CurSelStart = minimum(Cursor.m_SelectionStart, Cursor.m_SelectionEnd);
						m_CurSelEnd = maximum(Cursor.m_SelectionStart, Cursor.m_SelectionEnd);
					}
					if(m_CurSelStart != m_CurSelEnd)
					{
						if(WantsSelectionCopy)
						{
							bool HasNewLine = false;
							if(!SelectionString.empty())
								HasNewLine = true;
							int OffUTF8Start = 0;
							int OffUTF8End = 0;
							if(TextRender()->SelectionToUTF8OffSets(pEntry->m_aText, m_CurSelStart, m_CurSelEnd, OffUTF8Start, OffUTF8End))
							{
								SelectionString.insert(0, (std::string(&pEntry->m_aText[OffUTF8Start], OffUTF8End - OffUTF8Start) + (HasNewLine ? "\n" : "")));
							}
						}
						m_HasSelection = true;
					}
				}
				pEntry = pConsole->m_Backlog.Prev(pEntry);

				// reset color
				TextRender()->TextColor(1, 1, 1, 1);
				if(m_NewLineCounter > 0)
					--m_NewLineCounter;
			}

			if(WantsSelectionCopy && !SelectionString.empty())
			{
				Input()->SetClipboardText(SelectionString.c_str());
			}

			if(!pEntry)
				break;
			TotalPages++;
		}
		pConsole->m_BacklogCurPage = clamp(pConsole->m_BacklogCurPage, 0, TotalPages - 1);
		s_LastActivePage = pConsole->m_BacklogCurPage;

		pConsole->m_BacklogLock.unlock();

		// render page
		char aBuf[128];
		TextRender()->TextColor(1, 1, 1, 1);
		str_format(aBuf, sizeof(aBuf), Localize("-Page %d-"), pConsole->m_BacklogCurPage + 1);
		TextRender()->Text(0, 10.0f, FontSize / 2.f, FontSize, aBuf, -1.0f);

		// render version
		str_copy(aBuf, "v" GAME_VERSION " on " CONF_PLATFORM_STRING " " CONF_ARCH_STRING, sizeof(aBuf));
		float Width = TextRender()->TextWidth(0, FontSize, aBuf, -1, -1.0f);
		TextRender()->Text(0, Screen.w - Width - 10.0f, FontSize / 2.f, FontSize, aBuf, -1.0f);
	}
}

void CGameConsole::OnMessage(int MsgType, void *pRawMsg)
{
}

bool CGameConsole::OnInput(IInput::CEvent Event)
{
	// accept input when opening, but not at first frame to discard the input that caused the console to open
	if(m_ConsoleState != CONSOLE_OPEN && (m_ConsoleState != CONSOLE_OPENING || m_StateChangeEnd == TimeNow() + m_StateChangeDuration))
		return false;
	if((Event.m_Key >= KEY_F1 && Event.m_Key <= KEY_F12) || (Event.m_Key >= KEY_F13 && Event.m_Key <= KEY_F24))
		return false;

	if(Event.m_Key == KEY_ESCAPE && (Event.m_Flags & IInput::FLAG_PRESS))
		Toggle(m_ConsoleType);
	else
		CurrentConsole()->OnInput(Event);

	return true;
}

void CGameConsole::Toggle(int Type)
{
	if(m_ConsoleType != Type && (m_ConsoleState == CONSOLE_OPEN || m_ConsoleState == CONSOLE_OPENING))
	{
		// don't toggle console, just switch what console to use
	}
	else
	{
		if(m_ConsoleState == CONSOLE_CLOSED || m_ConsoleState == CONSOLE_OPEN)
		{
			m_StateChangeEnd = TimeNow() + m_StateChangeDuration;
		}
		else
		{
			float Progress = m_StateChangeEnd - TimeNow();
			float ReversedProgress = m_StateChangeDuration - Progress;

			m_StateChangeEnd = TimeNow() + ReversedProgress;
		}

		if(m_ConsoleState == CONSOLE_CLOSED || m_ConsoleState == CONSOLE_CLOSING)
		{
			/*Input()->MouseModeAbsolute();*/
			m_pClient->m_Menus.UseMouseButtons(false);
			m_ConsoleState = CONSOLE_OPENING;
			/*// reset controls
			m_pClient->m_Controls.OnReset();*/

			Input()->SetIMEState(true);
		}
		else
		{
			Input()->MouseModeRelative();
			m_pClient->m_Menus.UseMouseButtons(true);
			m_pClient->OnRelease();
			m_ConsoleState = CONSOLE_CLOSING;

			Input()->SetIMEState(false);
		}
	}
	if(m_ConsoleType != Type)
		m_HasSelection = false;
	m_ConsoleType = Type;
}

void CGameConsole::Dump(int Type)
{
	CInstance *pConsole = Type == CONSOLETYPE_REMOTE ? &m_RemoteConsole : &m_LocalConsole;
	char aFilename[IO_MAX_PATH_LENGTH];
	char aDate[20];

	str_timestamp(aDate, sizeof(aDate));
	str_format(aFilename, sizeof(aFilename), "dumps/%s_dump_%s.txt", Type == CONSOLETYPE_REMOTE ? "remote_console" : "local_console", aDate);
	IOHANDLE io = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(io)
	{
		pConsole->m_BacklogLock.lock();
		for(CInstance::CBacklogEntry *pEntry = pConsole->m_Backlog.First(); pEntry; pEntry = pConsole->m_Backlog.Next(pEntry))
		{
			io_write(io, pEntry->m_aText, str_length(pEntry->m_aText));
			io_write_newline(io);
		}
		pConsole->m_BacklogLock.unlock();
		io_close(io);
	}
}

void CGameConsole::ConToggleLocalConsole(IConsole::IResult *pResult, void *pUserData)
{
	((CGameConsole *)pUserData)->Toggle(CONSOLETYPE_LOCAL);
}

void CGameConsole::ConToggleRemoteConsole(IConsole::IResult *pResult, void *pUserData)
{
	((CGameConsole *)pUserData)->Toggle(CONSOLETYPE_REMOTE);
}

void CGameConsole::ConClearLocalConsole(IConsole::IResult *pResult, void *pUserData)
{
	((CGameConsole *)pUserData)->m_LocalConsole.ClearBacklog();
}

void CGameConsole::ConClearRemoteConsole(IConsole::IResult *pResult, void *pUserData)
{
	((CGameConsole *)pUserData)->m_RemoteConsole.ClearBacklog();
}

void CGameConsole::ConDumpLocalConsole(IConsole::IResult *pResult, void *pUserData)
{
	((CGameConsole *)pUserData)->Dump(CONSOLETYPE_LOCAL);
}

void CGameConsole::ConDumpRemoteConsole(IConsole::IResult *pResult, void *pUserData)
{
	((CGameConsole *)pUserData)->Dump(CONSOLETYPE_REMOTE);
}

void CGameConsole::ConConsolePageUp(IConsole::IResult *pResult, void *pUserData)
{
	CInstance *pConsole = ((CGameConsole *)pUserData)->CurrentConsole();
	pConsole->m_BacklogCurPage++;
}

void CGameConsole::ConConsolePageDown(IConsole::IResult *pResult, void *pUserData)
{
	CInstance *pConsole = ((CGameConsole *)pUserData)->CurrentConsole();
	--pConsole->m_BacklogCurPage;
	if(pConsole->m_BacklogCurPage < 0)
		pConsole->m_BacklogCurPage = 0;
}

void CGameConsole::RequireUsername(bool UsernameReq)
{
	if((m_RemoteConsole.m_UsernameReq = UsernameReq))
	{
		m_RemoteConsole.m_aUser[0] = '\0';
		m_RemoteConsole.m_UserGot = false;
	}
}

void CGameConsole::PrintLine(int Type, const char *pLine)
{
	if(Type == CONSOLETYPE_LOCAL)
		m_LocalConsole.PrintLine(pLine, str_length(pLine), ColorRGBA{1, 1, 1, 1});
	else if(Type == CONSOLETYPE_REMOTE)
		m_RemoteConsole.PrintLine(pLine, str_length(pLine), ColorRGBA{1, 1, 1, 1});
}

void CGameConsole::OnConsoleInit()
{
	// init console instances
	m_LocalConsole.Init(this);
	m_RemoteConsole.Init(this);

	m_pConsole = Kernel()->RequestInterface<IConsole>();

	Console()->Register("toggle_local_console", "", CFGFLAG_CLIENT, ConToggleLocalConsole, this, "Toggle local console");
	Console()->Register("toggle_remote_console", "", CFGFLAG_CLIENT, ConToggleRemoteConsole, this, "Toggle remote console");
	Console()->Register("clear_local_console", "", CFGFLAG_CLIENT, ConClearLocalConsole, this, "Clear local console");
	Console()->Register("clear_remote_console", "", CFGFLAG_CLIENT, ConClearRemoteConsole, this, "Clear remote console");
	Console()->Register("dump_local_console", "", CFGFLAG_CLIENT, ConDumpLocalConsole, this, "Dump local console");
	Console()->Register("dump_remote_console", "", CFGFLAG_CLIENT, ConDumpRemoteConsole, this, "Dump remote console");

	Console()->Register("console_page_up", "", CFGFLAG_CLIENT, ConConsolePageUp, this, "Previous page in console");
	Console()->Register("console_page_down", "", CFGFLAG_CLIENT, ConConsolePageDown, this, "Next page in console");
}

void CGameConsole::OnInit()
{
	m_pConsoleLogger = new CConsoleLogger(this);
	Engine()->SetAdditionalLogger(std::unique_ptr<ILogger>(m_pConsoleLogger));
	// add resize event
	Graphics()->AddWindowResizeListener([this](void *) {
		m_LocalConsole.ClearBacklogYOffsets();
		m_RemoteConsole.ClearBacklogYOffsets();
		m_HasSelection = false;
	},
		nullptr);
}

void CGameConsole::OnStateChange(int NewState, int OldState)
{
	if(OldState == IClient::STATE_ONLINE && NewState < IClient::STATE_LOADING)
	{
		m_RemoteConsole.m_UserGot = false;
		m_RemoteConsole.m_aUser[0] = '\0';
		m_RemoteConsole.m_Input.Clear();
		m_RemoteConsole.m_UsernameReq = false;
	}
}

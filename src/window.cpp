#include <cctype>
#include <cmath>
#include <sstream>

#include "buffer.h"
#include "display.h"
#include "mode.h"
#include "syntax.h"
#include "tab_window.h"
#include "theme.h"
#include "window.h"

#include "utils/logger.h"
#include "utils/stringutils.h"

// A 'window' is like a vim window; i.e. a region inside a tab
namespace Zep
{

//#define UTF8_CHAR_LEN(byte) ((0xE5000000 >> ((byte >> 3) & 0x1e)) & 3) + 1
#define UTF8_CHAR_LEN(byte) 1

namespace
{
const uint32_t Color_CursorNormal = 0xEEF35FBC;
const uint32_t Color_CursorInsert = 0xFFFFFFFF;
} // namespace

ZepWindow::ZepWindow(ZepTabWindow& window, ZepBuffer* buffer)
    : ZepComponent(window.GetEditor())
    , m_pBuffer(buffer)
    , m_tabWindow(window)
{
}

ZepWindow::~ZepWindow()
{
}

void ZepWindow::UpdateAirline()
{
    m_airline.leftBoxes.clear();
    m_airline.rightBoxes.clear();
    m_airline.leftBoxes.push_back(AirBox {GetEditor().GetCurrentMode()->Name(), 0xAAAA00AA } );
    switch(m_cursorMode)
    {
    case CursorMode::Hidden:
        m_airline.leftBoxes.push_back(AirBox{ "HIDDEN", 0xFF1111AA });
        break;
    case CursorMode::Insert:
        m_airline.leftBoxes.push_back(AirBox{ "INSERT", 0xFF1111AA });
        break;
    case CursorMode::Normal:
        m_airline.leftBoxes.push_back(AirBox{ "NORMAL", 0xFF1111AA });
        break;
    case CursorMode::Visual:
        m_airline.leftBoxes.push_back(AirBox{ "VISUAL", 0xFF1111AA });
        break;
    };
    m_airline.leftBoxes.push_back(AirBox{ m_pBuffer->GetFilePath().string(), 0xFF000000 });

    m_airline.rightBoxes.push_back(AirBox{std::to_string(m_pBuffer->GetLineEnds().size()) + " Lines", 0xFF11AAAA});
}

void ZepWindow::SetCursorMode(CursorMode mode)
{
    m_cursorMode = mode;
    GetEditor().ResetCursorTimer();

    UpdateAirline();
}

void ZepWindow::Notify(std::shared_ptr<ZepMessage> payload)
{
    if (payload->messageId == Msg_Buffer)
    {
        auto pMsg = std::static_pointer_cast<BufferMessage>(payload);

        if (pMsg->pBuffer != m_pBuffer)
        {
            return;
        }

        if (pMsg->type != BufferMessageType::PreBufferChange)
        {
            // Put the cursor where the replaced text was added
            GetEditor().ResetCursorTimer();
        }

        m_linesChanged = true;
    }
}

void ZepWindow::SetDisplayRegion(const DisplayRegion& region)
{
    if (m_bufferRegion == region)
    {
        return;
    }

    m_linesChanged = true;
    m_bufferRegion = region;

    // Account for airline
    auto statusCount = 1;// m_statusLines.size() + 1;
    const auto windowSize = m_bufferRegion.bottomRightPx - m_bufferRegion.topLeftPx;
    const float statusSize = GetEditor().GetDisplay().GetFontSize() * statusCount + textBorder * 2.0f;

    m_statusRegion.bottomRightPx = m_bufferRegion.bottomRightPx;
    m_statusRegion.topLeftPx = m_bufferRegion.bottomRightPx - NVec2f(windowSize.x, statusSize);

    auto lastRegion = m_textRegion;
    m_textRegion.bottomRightPx = m_statusRegion.topLeftPx + NVec2f(windowSize.x, 0.0f);
    m_textRegion.topLeftPx = m_bufferRegion.topLeftPx;

    // Border, and move the text across a bit
    m_leftRegion.topLeftPx = m_textRegion.topLeftPx;
    m_leftRegion.bottomRightPx = NVec2f(m_leftRegion.topLeftPx.x + leftBorder, m_textRegion.bottomRightPx.y);

    m_textRegion.topLeftPx.x += leftBorder + textBorder;

    m_defaultLineSize = GetEditor().GetDisplay().GetTextSize((Zep::utf8*)"A").y;

    m_bufferOffsetYPx = 0;

    UpdateAirline();
}

void ZepWindow::ScrollToCursor()
{
    bool changed = false;

    auto two_lines = (GetEditor().GetDisplay().GetFontSize() * 2);
    auto& cursorLine = GetCursorLineInfo(BufferToDisplay().y);

    if (m_bufferOffsetYPx > (cursorLine.spanYPx - two_lines))
    {
        m_bufferOffsetYPx -= (m_bufferOffsetYPx - (cursorLine.spanYPx - two_lines));
        changed = true;
    }
    else if ((m_bufferOffsetYPx + m_textRegion.Height() - two_lines) < cursorLine.spanYPx)
    {
        m_bufferOffsetYPx += cursorLine.spanYPx - (m_bufferOffsetYPx + m_textRegion.Height() - two_lines);
        changed = true;
    }

    if (changed)
    {
        m_bufferOffsetYPx = std::max(0.f, m_bufferOffsetYPx);
        UpdateVisibleLineRange();
    }
}

void ZepWindow::CheckLineSpans()
{
    // If changed, update
    if (!m_linesChanged)
    {
        return;
    }
    m_linesChanged = false;

    m_maxDisplayLines = (long)std::max(0.0f, std::floor((m_textRegion.bottomRightPx.y - m_textRegion.topLeftPx.y) / m_defaultLineSize));

    float screenPosX = m_textRegion.topLeftPx.x;

    BufferLocation loc = 0;

    // For now, we are compromising on ASCII; so don't query font size each time
    // The performance of the editor currently comes down to this function
    // It can be improved by incrementally updating the line spans, potentially threading, etc.
    // but it isn't necessary yet.
    const auto& textBuffer = m_pBuffer->GetText();

    long bufferLine = 0;
    long spanLine = 0;
    float bufferPosYPx = 0;

    auto ensureSpanLines = [&](long lines)
    {
        if (m_windowLines.size() <= lines)
        {
            m_windowLines.resize(lines + 1);
        }
    };

    // Process every buffer line
    for (;;)
    {
        // We haven't processed this line yet, so we can't display anything
        // else
        if (m_pBuffer->GetLineCount() <= bufferLine)
            break;

        NVec2i columnOffsets;
        if (!m_pBuffer->GetLineOffsets(bufferLine, columnOffsets.x, columnOffsets.y))
            break;

        SpanInfo* lineInfo = nullptr;

        // These offsets are 0 -> n + 1, i.e. the last offset the buffer returns is 1 beyond the current
        for (auto ch = columnOffsets.x; ch < columnOffsets.y; ch++)
        {
            const utf8* pCh = &textBuffer[ch];
            const auto textSize = GetEditor().GetDisplay().GetTextSize(pCh, pCh + 1);

            // Wrap if we have displayed at least one char, and we have to
            if (m_wrap && ch != columnOffsets.x)
            {
                if (((screenPosX + textSize.x) + textSize.x) >= (m_textRegion.bottomRightPx.x))
                {
                    // Remember the offset beyond the end of the line
                    lineInfo->columnOffsets.y = ch;

                    // Next line
                    spanLine++;
                    bufferPosYPx += GetEditor().GetDisplay().GetFontSize();

                    ensureSpanLines(spanLine);

                    lineInfo = &m_windowLines[spanLine];
                    lineInfo->charInfo.clear();

                    // Now jump to the next 'screen line' for the rest of this 'buffer line'
                    lineInfo->columnOffsets = NVec2i(ch, ch + 1);
                    lineInfo->lastNonCROffset = 0;
                    lineInfo->lineIndex = spanLine;
                    lineInfo->bufferLineNumber = bufferLine;
                    screenPosX = m_textRegion.topLeftPx.x;
                }
                else
                {
                    screenPosX += textSize.x;
                }
            }

            // Collect all display lines.
            // Lines before the first active line will have screen line number -1
            ensureSpanLines(spanLine);

            if (lineInfo == nullptr)
            {
                lineInfo = &m_windowLines[spanLine];
                lineInfo->bufferLineNumber = bufferLine;
                lineInfo->lineIndex = spanLine;
                lineInfo->spanYPx = bufferPosYPx;
                lineInfo->columnOffsets.x = ch;
                lineInfo->charInfo.clear();
            }

            lineInfo->columnOffsets.y = ch + 1;

            lineInfo->charInfo.push_back(CharInfo{});

            auto charIndex = lineInfo->charInfo.size() - 1;
            lineInfo->charInfo[charIndex].bufferLocation = ch;
            lineInfo->charInfo[charIndex].screenPosXPx = screenPosX;
            lineInfo->lastNonCROffset = std::max(ch, 0l);

            const utf8* pEnd = pCh + UTF8_CHAR_LEN(*pCh);
            lineInfo->charInfo[charIndex].bufferLocationEnd = UTF8_CHAR_LEN(*pCh);
            lineInfo->charInfo[charIndex].textSize = textSize;
        }

        // Next time round - down a buffer line, down a span line
        bufferLine++;
        spanLine++;
        screenPosX = m_textRegion.topLeftPx.x;
        bufferPosYPx += GetEditor().GetDisplay().GetFontSize();
        lineInfo = nullptr;
    }

     // Ensure we aren't too big
     m_windowLines.resize(spanLine);

    // Sanity
    if (m_windowLines.empty())
    {
        SpanInfo lineInfo;
        lineInfo.columnOffsets.x = 0;
        lineInfo.columnOffsets.y = 0;
        lineInfo.lastNonCROffset = 0;
        lineInfo.bufferLineNumber = 0;
        m_windowLines.push_back(lineInfo);
    }

    UpdateVisibleLineRange();
}

void ZepWindow::UpdateVisibleLineRange()
{
    m_visibleLineRange.x = (long)m_windowLines.size();
    m_visibleLineRange.y = 0;
    for (long line = 0; line < long(m_windowLines.size()); line++)
    {
        auto& windowLine = m_windowLines[line];
        if ((windowLine.spanYPx + GetEditor().GetDisplay().GetFontSize()) < m_bufferOffsetYPx)
        {
            continue;
        }

        if ((windowLine.spanYPx - m_bufferOffsetYPx) >= m_textRegion.bottomRightPx.y)
        {
            break;
        }

        m_visibleLineRange.x = std::min(m_visibleLineRange.x, long(line));
        m_visibleLineRange.y = long(line);
    }
    m_visibleLineRange.y++;

    //LOG(DEBUG) << "Line Range: " << std::to_string(m_visibleLineRange.x) + ", " + std::to_string(m_visibleLineRange.y);
}

const SpanInfo& ZepWindow::GetCursorLineInfo(long y)
{
    CheckLineSpans();
    y = std::max(0l, y);
    y = std::min(y, long(m_windowLines.size() - 1));
    return m_windowLines[y];
}

// TODO: This function draws one char at a time.  It could be more optimal at the expense of some
// complexity.  Basically, I don't like the current implementation, but it works for now.
// The text is displayed acorrding to the region bounds and the display lineData
// Additionally (and perhaps that should be a seperate function), this code draws line numbers
bool ZepWindow::DisplayLine(const SpanInfo& lineInfo, const DisplayRegion& region, int displayPass)
{
    // A middle-dot whitespace character
    static const auto whiteSpace = StringUtils::makeStr(std::wstring(L"\x00b7"));
    static const auto blankSpace = ' ';

    auto activeWindow = (GetEditor().GetActiveTabWindow()->GetActiveWindow() == this);
    auto cursorCL = BufferToDisplay();

    auto ToRegionY = [&](float pos)
    {
        return pos - m_bufferOffsetYPx + region.topLeftPx.y;
    };

    // Draw line numbers
    auto showLineNumber = [&]() {
        if (!IsInsideTextRegion(NVec2i(0, lineInfo.lineIndex)))
            return;
        auto cursorBufferLine = GetCursorLineInfo(cursorCL.y).bufferLineNumber;
        std::string strNum;
        if (m_displayMode == DisplayMode::Vim)
        {
            strNum = std::to_string(abs(lineInfo.bufferLineNumber - cursorBufferLine));
        }
        else
        {
            strNum = std::to_string(lineInfo.bufferLineNumber);
        }
        auto textSize = GetEditor().GetDisplay().GetTextSize((const utf8*)strNum.c_str(), (const utf8*)(strNum.c_str() + strNum.size()));

        // Number background
        GetEditor().GetDisplay().DrawRectFilled(NVec2f(m_leftRegion.topLeftPx.x, ToRegionY(lineInfo.spanYPx)),
            NVec2f(m_leftRegion.bottomRightPx.x, ToRegionY(lineInfo.spanYPx) + GetEditor().GetDisplay().GetFontSize()),
            0xFF222222);

        auto digitCol = 0xFF11FF11;
        if (lineInfo.BufferCursorInside(m_bufferCursor))
        {
            digitCol = Color_CursorNormal;
        }

        // Numbers
        GetEditor().GetDisplay().DrawChars(NVec2f(m_leftRegion.bottomRightPx.x - textSize.x - textBorder, ToRegionY(lineInfo.spanYPx)), digitCol,
            (const utf8*)strNum.c_str(),
            (const utf8*)(strNum.c_str() + strNum.size()));
    };

    if (displayPass == WindowPass::Background)
    {
        showLineNumber();
    }

    bool foundCR = false;
    auto screenPosX = m_textRegion.topLeftPx.x;

    char invalidChar;

    // Walk from the start of the line to the end of the line (in buffer chars)
    for (auto ch = lineInfo.columnOffsets.x; ch < lineInfo.columnOffsets.y; ch++)
    {
        auto& info = lineInfo.charInfo[ch - lineInfo.columnOffsets.x];
        auto pSyntax = m_pBuffer->GetSyntax();
        auto col = pSyntax != nullptr ? Theme::Instance().GetColor(pSyntax->GetSyntaxAt(info.bufferLocation)) : 0xFFFFFFFF;

        const utf8* pCh = &m_pBuffer->GetText()[info.bufferLocation];

        // Visible white space
        if (pSyntax && pSyntax->GetSyntaxAt(info.bufferLocation) == SyntaxType::Whitespace)
        {
            pCh = (const utf8*)whiteSpace.c_str();
        }

        // Shown only one char for end of line
        if (*pCh == '\n' || *pCh == 0)
        {
            invalidChar = '@' + *pCh;
            if (m_windowFlags & WindowFlags::ShowCR)
            {
                pCh = (const utf8*)&invalidChar;
            }
            else
            {
                pCh = (const utf8*)&blankSpace;
            }
            col = 0x771111FF;
        }
        const utf8* pEnd = pCh + UTF8_CHAR_LEN(*pCh);

        if (displayPass == 0)
        {
            if (activeWindow)
            {
                if (m_cursorMode == CursorMode::Visual)
                {
                    if (info.bufferLocation >= m_selection.start && info.bufferLocation <= m_selection.end)
                    {
                        GetEditor().GetDisplay().DrawRectFilled(NVec2f(screenPosX, ToRegionY(lineInfo.spanYPx)), NVec2f(screenPosX + info.textSize.x, ToRegionY(lineInfo.spanYPx) + info.textSize.y), 0xFF784F26);
                    }
                }
            }
        }
        else
        {
            if (pSyntax && pSyntax->GetSyntaxAt(info.bufferLocation) == SyntaxType::Whitespace)
            {
                auto centerChar = NVec2f(screenPosX + info.textSize.x / 2, ToRegionY(lineInfo.spanYPx) + info.textSize.y / 2);
                GetEditor().GetDisplay().DrawRectFilled(centerChar - NVec2f(1.0f, 1.0f), centerChar + NVec2f(1.0f, 1.0f), 0xFF524814);
            }
            else
            {
                GetEditor().GetDisplay().DrawChars(NVec2f(screenPosX, ToRegionY(lineInfo.spanYPx)), col,
                    pCh,
                    pEnd);
            }
        }

        screenPosX += info.textSize.x;
    }

    DisplayCursor();

    return true;
} // namespace Zep

bool ZepWindow::IsInsideTextRegion(NVec2i pos) const
{
    if (pos.y < m_visibleLineRange.x || pos.y >= m_visibleLineRange.y)
    {
        return false;
    }
    return true;
}

void ZepWindow::DisplayCursor()
{
    auto activeWindow = (GetEditor().GetActiveTabWindow()->GetActiveWindow() == this);
    auto cursorCL = BufferToDisplay();

    // Draw the cursor
    auto cursorBufferLine = GetCursorLineInfo(cursorCL.y);

    if (!IsInsideTextRegion(cursorCL))
    {
        return;
    }

    CharInfo* pCharInfo = nullptr;
    CharInfo lastPos;
    if (cursorCL.x < cursorBufferLine.charInfo.size())
    {
        pCharInfo = &cursorBufferLine.charInfo[cursorCL.x];
    }
    else
    {
        if (!cursorBufferLine.charInfo.empty())
        {
            lastPos = cursorBufferLine.charInfo[cursorBufferLine.charInfo.size() - 1];
            lastPos.screenPosXPx += lastPos.textSize.x;
        }
        else
        {
            lastPos.screenPosXPx = 0;
        }
        lastPos.textSize = GetEditor().GetDisplay().GetTextSize((Zep::utf8*)"A");
        pCharInfo = &lastPos;
    }

    // Cursor
    auto cursorPosPx = NVec2f(pCharInfo->screenPosXPx, cursorBufferLine.spanYPx - m_bufferOffsetYPx + m_textRegion.topLeftPx.y);
    auto cursorBlink = GetEditor().GetCursorBlinkState();
    if (!cursorBlink)
    {
        switch (m_cursorMode)
        {
        default:
        case CursorMode::Hidden:
            break;

        case CursorMode::Insert:
        {
            GetEditor().GetDisplay().DrawRectFilled(NVec2f(cursorPosPx.x - 1, cursorPosPx.y), NVec2f(cursorPosPx.x, cursorPosPx.y + pCharInfo->textSize.y), 0xEEFFFFFF);
        }
        break;

        case CursorMode::Normal:
        case CursorMode::Visual:
        {
            GetEditor().GetDisplay().DrawRectFilled(cursorPosPx, NVec2f(cursorPosPx.x + pCharInfo->textSize.x, cursorPosPx.y + pCharInfo->textSize.y), Color_CursorNormal);
        }
        break;
        }
    }
}

ZepTabWindow& ZepWindow::GetTabWindow() const
{
    return m_tabWindow;
}

void ZepWindow::SetWindowFlags(uint32_t windowFlags)
{
    m_windowFlags = windowFlags;
}

uint32_t ZepWindow::GetWindowFlags() const
{
    return m_windowFlags;
}

void ZepWindow::ToggleFlag(uint32_t flag)
{
    if (m_windowFlags & flag)
    {
        m_windowFlags &= ~flag;
    }
    else
    {
        m_windowFlags |= flag;
    }
}

long ZepWindow::GetMaxDisplayLines()
{
    CheckLineSpans();
    return m_maxDisplayLines;
}

long ZepWindow::GetNumDisplayedLines()
{
    CheckLineSpans();
    return std::min((long)m_windowLines.size(), GetMaxDisplayLines());
}

void ZepWindow::SetBufferCursor(BufferLocation location)
{
    m_bufferCursor = m_pBuffer->Clamp(location);
    m_lastCursorColumn = m_pBuffer->GetBufferColumn(location);
}

void ZepWindow::SetSelectionRange(BufferLocation start, BufferLocation end)
{
    m_selection.start = start;
    m_selection.end = end;
    m_selection.vertical = false;
    if (m_selection.start > m_selection.end)
    {
        std::swap(m_selection.start, m_selection.end);
    }
}

void ZepWindow::SetBuffer(ZepBuffer* pBuffer)
{
    assert(pBuffer);
    m_pBuffer = pBuffer;
    m_linesChanged = true;
}

BufferLocation ZepWindow::GetBufferCursor()
{
    return m_bufferCursor;
}

ZepBuffer& ZepWindow::GetBuffer() const
{
    return *m_pBuffer;
}

void ZepWindow::Display()
{
    // Ensure line spans are valid; updated if the text is changed or the window dimensions change
    CheckLineSpans();

    ScrollToCursor();

    auto cursorCL = BufferToDisplay(m_bufferCursor);

    auto activeWindow = (GetEditor().GetActiveTabWindow()->GetActiveWindow() == this);

    if (activeWindow && cursorCL.x != -1)
    {
        if (m_cursorMode == CursorMode::Normal || m_cursorMode == CursorMode::Insert)
        {
            auto& cursorLine = GetCursorLineInfo(cursorCL.y);

            if (IsInsideTextRegion(cursorCL))
            {
                // Cursor line
                GetEditor().GetDisplay().DrawRectFilled(NVec2f(m_textRegion.topLeftPx.x, cursorLine.spanYPx - m_bufferOffsetYPx + m_textRegion.topLeftPx.y),
                    NVec2f(m_textRegion.bottomRightPx.x, cursorLine.spanYPx - m_bufferOffsetYPx + m_textRegion.topLeftPx.y + GetEditor().GetDisplay().GetFontSize()),
                    0xFF222222);
            }
        }
    }

    for (int displayPass = 0; displayPass < WindowPass::Max; displayPass++)
    {
        for (long windowLine = m_visibleLineRange.x; windowLine < m_visibleLineRange.y; windowLine++)
        {
            auto& lineInfo = m_windowLines[windowLine];
            if (!DisplayLine(lineInfo, m_textRegion, displayPass))
            {
                break;
            }
        }
    }

    // Airline
    GetEditor().GetDisplay().DrawRectFilled(m_statusRegion.topLeftPx, m_statusRegion.bottomRightPx, 0xFF222222);
    auto airHeight = GetEditor().GetDisplay().GetFontSize();
    auto border = 6.0f;
    
    NVec2f screenPosYPx = m_statusRegion.topLeftPx + NVec2f(0.0f, textBorder);
    for (int i = 0; i < m_airline.leftBoxes.size(); i++)
    {
        auto textSize = GetEditor().GetDisplay().GetTextSize((const utf8*)m_airline.leftBoxes[i].text.c_str());
        textSize.x += border * 2;
        GetEditor().GetDisplay().DrawRectFilled(screenPosYPx, NVec2f(textSize.x + screenPosYPx.x, screenPosYPx.y + airHeight), m_airline.leftBoxes[i].background);
        GetEditor().GetDisplay().DrawChars(screenPosYPx + NVec2f(border, 0),
            0xFFFFFFFF,
            (const utf8*)(m_airline.leftBoxes[i].text.c_str()));
        screenPosYPx.x += textSize.x;
    }
  
}

// *** Motions ***
void ZepWindow::MoveCursorY(int yDistance, LineLocation clampLocation)
{
    CheckLineSpans();

    // Get the cursor
    auto cursorCL = BufferToDisplay();
    if (cursorCL.x == -1)
        return;

    // Find the screen line relative target
    auto target = cursorCL + NVec2i(0, yDistance);
    target.y = std::max(0l, target.y);
    target.y = std::min(target.y, long(m_windowLines.size() - 1));

    auto& line = m_windowLines[target.y];

    // Snap to the new vertical column if necessary (see comment below)
    if (target.x < m_lastCursorColumn)
        target.x = m_lastCursorColumn;

    // Update the master buffer cursor
    m_bufferCursor = line.columnOffsets.x + target.x;

    // Ensure the current x offset didn't walk us off the line (column offset is 1 beyond, and there is a single \n before it)
    // We are clamping to visible line here
    m_bufferCursor = std::min(m_bufferCursor, line.columnOffsets.y - 2);
    m_bufferCursor = std::max(m_bufferCursor, line.columnOffsets.x);

    GetEditor().ResetCursorTimer();
}

NVec2i ZepWindow::BufferToDisplay()
{
    return BufferToDisplay(m_bufferCursor);
}

NVec2i ZepWindow::BufferToDisplay(const BufferLocation& loc)
{
    CheckLineSpans();

    NVec2i ret(0, 0);
    int line_number = 0;

    // TODO:  A map
    for (auto& line : m_windowLines)
    {
        if (line.columnOffsets.x <= loc && line.columnOffsets.y > loc)
        {
            ret.y = line_number;
            ret.x = loc - line.columnOffsets.x;
            return ret;
        }
        line_number++;
    }

    // Max
    ret.y = int(m_windowLines.size() - 1);
    ret.x = m_windowLines[m_windowLines.size() - 1].columnOffsets.y - 1;
    return ret;
}

} // namespace Zep

/*
    // Ensure we can see the cursor
    /*
    NVec2i cursor(0, 0);
    cursor.x = m_pBuffer->GetBufferColumn(m_bufferCursor);
    cursor.y = m_pBuffer->GetBufferLine(m_bufferCursor) - m_pBuffer->GetBufferLine(m_dvisibleLineRange.x);

    // Handle the case where there is no need to scroll, since the visible lines are inside
    // The current screen rectangle.
    if (cursor.y >= m_nvisibleLineRange.y && m_linesFillScreen)
    {
        m_visibleLineRange.x = cursor.y - (m_nvisibleLineRange.y - m_vnisibleLineRange.x) + 1;
        m_linesChanged = true;
    }
    else if (cursor.y < m_nvisibleLineRange.x)
    {
        m_nvisibleLineRange.x = cursor.y;
        m_linesChanged = true;
    }

    // Clamp
    m_nvisibleLineRange.x = std::max(0l, (long)m_nvisibleLineRange.x);
*/

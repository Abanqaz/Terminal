/********************************************************
*                                                       *
*   Copyright (C) Microsoft. All rights reserved.       *
*                                                       *
********************************************************/

#include "precomp.h"

#include "getset.h"

#include "_output.h"
#include "_stream.h"
#include "output.h"
#include "dbcs.h"
#include "handle.h"
#include "misc.h"
#include "cmdline.h"

#include "../types/inc/convert.hpp"
#include "../types/inc/viewport.hpp"

#include "ApiRoutines.h"

#include "..\interactivity\inc\ServiceLocator.hpp"

#pragma hdrstop

// The following mask is used to test for valid text attributes.
#define VALID_TEXT_ATTRIBUTES (FG_ATTRS | BG_ATTRS | META_ATTRS)

#define INPUT_MODES (ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT)
#define OUTPUT_MODES (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN | ENABLE_LVB_GRID_WORLDWIDE)
#define PRIVATE_MODES (ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE | ENABLE_AUTO_POSITION | ENABLE_EXTENDED_FLAGS)

using namespace Microsoft::Console::Types;

void ApiRoutines::GetConsoleInputModeImpl(InputBuffer& context, ULONG& mode) noexcept
{
    try
    {
        Telemetry::Instance().LogApiCall(Telemetry::ApiCall::GetConsoleMode);
        const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        mode = context.InputMode;

        if (WI_IsFlagSet(gci.Flags, CONSOLE_USE_PRIVATE_FLAGS))
        {
            WI_SetFlag(mode, ENABLE_EXTENDED_FLAGS);
            WI_SetFlagIf(mode, ENABLE_INSERT_MODE, gci.GetInsertMode());
            WI_SetFlagIf(mode, ENABLE_QUICK_EDIT_MODE, WI_IsFlagSet(gci.Flags, CONSOLE_QUICK_EDIT_MODE));
            WI_SetFlagIf(mode, ENABLE_AUTO_POSITION, WI_IsFlagSet(gci.Flags, CONSOLE_AUTO_POSITION));
        }
    }
    CATCH_LOG();
}

void ApiRoutines::GetConsoleOutputModeImpl(SCREEN_INFORMATION& context, ULONG& mode) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        mode = context.GetActiveBuffer().OutputMode;
    }
    CATCH_LOG();
}

[[nodiscard]]
HRESULT ApiRoutines::GetNumberOfConsoleInputEventsImpl(const InputBuffer& context, ULONG& events) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        size_t readyEventCount = context.GetNumberOfReadyEvents();
        RETURN_IF_FAILED(SizeTToULong(readyEventCount, &events));

        return S_OK;
    }
    CATCH_RETURN();
}

void ApiRoutines::GetConsoleScreenBufferInfoExImpl(const SCREEN_INFORMATION& context,
                                                   CONSOLE_SCREEN_BUFFER_INFOEX& data) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        data.bFullscreenSupported = FALSE; // traditional full screen with the driver support is no longer supported.
        context.GetScreenBufferInformation(&data.dwSize,
                                           &data.dwCursorPosition,
                                           &data.srWindow,
                                           &data.wAttributes,
                                           &data.dwMaximumWindowSize,
                                           &data.wPopupAttributes,
                                           data.ColorTable);
        // Callers of this function expect to recieve an exclusive rect, not an inclusive one.
        data.srWindow.Right += 1;
        data.srWindow.Bottom += 1;
    }
    CATCH_LOG();
}

void ApiRoutines::GetConsoleCursorInfoImpl(const SCREEN_INFORMATION& context,
                                           ULONG& size,
                                           bool& isVisible) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        size = context.GetTextBuffer().GetCursor().GetSize();
        isVisible = context.GetTextBuffer().GetCursor().IsVisible();
    }
    CATCH_LOG();
}

void ApiRoutines::GetConsoleSelectionInfoImpl(CONSOLE_SELECTION_INFO& consoleSelectionInfo) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        const Selection* const pSelection = &Selection::Instance();
        if (pSelection->IsInSelectingState())
        {
            consoleSelectionInfo.dwFlags = pSelection->GetPublicSelectionFlags();

            WI_SetFlag(consoleSelectionInfo.dwFlags, CONSOLE_SELECTION_IN_PROGRESS);

            consoleSelectionInfo.dwSelectionAnchor = pSelection->GetSelectionAnchor();
            consoleSelectionInfo.srSelection = pSelection->GetSelectionRectangle();
        }
        else
        {
            ZeroMemory(&consoleSelectionInfo, sizeof(consoleSelectionInfo));
        }
    }
    CATCH_LOG();
}

void ApiRoutines::GetNumberOfConsoleMouseButtonsImpl(ULONG& buttons) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        buttons = ServiceLocator::LocateSystemConfigurationProvider()->GetNumberOfMouseButtons();
    }
    CATCH_LOG();
}

[[nodiscard]]
HRESULT ApiRoutines::GetConsoleFontSizeImpl(const SCREEN_INFORMATION& context,
                                            const DWORD index,
                                            COORD& size) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        if (index == 0)
        {
            // As of the November 2015 renderer system, we only have a single font at index 0.
            size = context.GetActiveBuffer().GetTextBuffer().GetCurrentFont().GetUnscaledSize();
            return S_OK;
        }
        else
        {
            // Invalid font is 0,0 with STATUS_INVALID_PARAMETER
            size = { 0 };
            return E_INVALIDARG;
        }
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::GetCurrentConsoleFontExImpl(const SCREEN_INFORMATION& context,
                                                 const bool isForMaximumWindowSize,
                                                 CONSOLE_FONT_INFOEX& consoleFontInfoEx) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        const SCREEN_INFORMATION& activeScreenInfo = context.GetActiveBuffer();

        COORD WindowSize;
        if (isForMaximumWindowSize)
        {
            WindowSize = activeScreenInfo.GetMaxWindowSizeInCharacters();
        }
        else
        {
            WindowSize = activeScreenInfo.GetTextBuffer().GetCurrentFont().GetUnscaledSize();
        }
        consoleFontInfoEx.dwFontSize = WindowSize;

        consoleFontInfoEx.nFont = 0;

        const FontInfo& fontInfo = activeScreenInfo.GetTextBuffer().GetCurrentFont();
        consoleFontInfoEx.FontFamily = fontInfo.GetFamily();
        consoleFontInfoEx.FontWeight = fontInfo.GetWeight();

        RETURN_IF_FAILED(StringCchCopyW(consoleFontInfoEx.FaceName, ARRAYSIZE(consoleFontInfoEx.FaceName), fontInfo.GetFaceName()));

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetCurrentConsoleFontExImpl(IConsoleOutputObject& context,
                                                 const bool /*isForMaximumWindowSize*/,
                                                 const CONSOLE_FONT_INFOEX& consoleFontInfoEx) noexcept
{
    try
    {
        const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        SCREEN_INFORMATION& activeScreenInfo = context.GetActiveBuffer();

        WCHAR FaceName[ARRAYSIZE(consoleFontInfoEx.FaceName)];
        RETURN_IF_FAILED(StringCchCopyW(FaceName, ARRAYSIZE(FaceName), consoleFontInfoEx.FaceName));

        FontInfo fi(FaceName,
                    static_cast<BYTE>(consoleFontInfoEx.FontFamily),
                    consoleFontInfoEx.FontWeight,
                    consoleFontInfoEx.dwFontSize,
                    gci.OutputCP);

        // TODO: MSFT: 9574827 - should this have a failure case?
        activeScreenInfo.UpdateFont(&fi);

        // If this is the active screen buffer, also cause the window to refresh its viewport size.
        if (activeScreenInfo.IsActiveScreenBuffer())
        {
            IConsoleWindow* const pWindow = ServiceLocator::LocateConsoleWindow();
            if (nullptr != pWindow)
            {
                pWindow->PostUpdateWindowSize();
            }
        }

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleInputModeImpl(InputBuffer& context, const ULONG mode) noexcept
{
    try
    {
        CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        if (WI_IsAnyFlagSet(mode, PRIVATE_MODES))
        {
            WI_SetFlag(gci.Flags, CONSOLE_USE_PRIVATE_FLAGS);

            WI_UpdateFlag(gci.Flags, CONSOLE_QUICK_EDIT_MODE, WI_IsFlagSet(mode, ENABLE_QUICK_EDIT_MODE));
            WI_UpdateFlag(gci.Flags, CONSOLE_AUTO_POSITION, WI_IsFlagSet(mode, ENABLE_AUTO_POSITION));

            const bool PreviousInsertMode = gci.GetInsertMode();
            gci.SetInsertMode(WI_IsFlagSet(mode, ENABLE_INSERT_MODE));
            if (gci.GetInsertMode() != PreviousInsertMode)
            {
                gci.GetActiveOutputBuffer().SetCursorDBMode(false);
                if (gci.HasPendingCookedRead())
                {
                    gci.CookedReadData().SetInsertMode(gci.GetInsertMode());
                }
            }
        }
        else
        {
            WI_ClearFlag(gci.Flags, CONSOLE_USE_PRIVATE_FLAGS);
        }

        context.InputMode = mode;
        WI_ClearAllFlags(context.InputMode, PRIVATE_MODES);

        // NOTE: For compatibility reasons, we need to set the modes and then return the error codes, not the other way around
        //       as might be expected.
        //       This is a bug from a long time ago and some applications depend on this functionality to operate properly.
        //       ---
        //       A prime example of this is that PSReadline module in Powershell will set the invalid mode 0x1e4
        //       which includes 0x4 for ECHO_INPUT but turns off 0x2 for LINE_INPUT. This is invalid, but PSReadline
        //       relies on it to properly receive the ^C printout and make a new line when the user presses Ctrl+C.
        {
            // Flags we don't understand are invalid.
            RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(mode, ~(INPUT_MODES | PRIVATE_MODES)));

            // ECHO on with LINE off is invalid.
            RETURN_HR_IF(E_INVALIDARG, WI_IsFlagSet(mode, ENABLE_ECHO_INPUT) && WI_IsFlagClear(mode, ENABLE_LINE_INPUT));
        }

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleOutputModeImpl(SCREEN_INFORMATION& context, const ULONG mode) noexcept
{
    try
    {
        CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        // Flags we don't understand are invalid.
        RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(mode, ~OUTPUT_MODES));

        SCREEN_INFORMATION& screenInfo = context.GetActiveBuffer();
        const DWORD dwOldMode = screenInfo.OutputMode;
        const DWORD dwNewMode = mode;

        screenInfo.OutputMode = dwNewMode;

        // if we're moving from VT on->off
        if (WI_IsFlagClear(dwNewMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING) && WI_IsFlagSet(dwOldMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        {
            // jiggle the handle
            screenInfo.GetStateMachine().ResetState();
            screenInfo.ClearTabStops();
        }
        // if we're moving from VT off->on
        else if (WI_IsFlagSet(dwNewMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING) &&
                 WI_IsFlagClear(dwOldMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        {
            screenInfo.SetDefaultVtTabStops();
        }

        gci.SetVirtTermLevel(WI_IsFlagSet(dwNewMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING) ? 1 : 0);
        gci.SetAutomaticReturnOnNewline(WI_IsFlagSet(screenInfo.OutputMode, DISABLE_NEWLINE_AUTO_RETURN) ? false : true);
        gci.SetGridRenderingAllowedWorldwide(WI_IsFlagSet(screenInfo.OutputMode, ENABLE_LVB_GRID_WORLDWIDE));

        // if we changed rendering modes then redraw the output buffer
        if (WI_IsFlagSet(dwNewMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING) != WI_IsFlagSet(dwOldMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
            WI_IsFlagSet(dwNewMode, ENABLE_LVB_GRID_WORLDWIDE) != WI_IsFlagSet(dwOldMode, ENABLE_LVB_GRID_WORLDWIDE))
        {
            auto* pRender = ServiceLocator::LocateGlobals().pRender;
            if (pRender)
            {
                pRender->TriggerRedrawAll();
            }
        }

        return S_OK;
    }
    CATCH_RETURN();
}

void ApiRoutines::SetConsoleActiveScreenBufferImpl(SCREEN_INFORMATION& newContext) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        SetActiveScreenBuffer(newContext.GetActiveBuffer());
    }
    CATCH_LOG();
}

void ApiRoutines::FlushConsoleInputBuffer(InputBuffer& context) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        context.Flush();
    }
    CATCH_LOG();
}

void ApiRoutines::GetLargestConsoleWindowSizeImpl(const SCREEN_INFORMATION& context,
                                                  COORD& size) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        const SCREEN_INFORMATION& screenInfo = context.GetActiveBuffer();

        size = screenInfo.GetLargestWindowSizeInCharacters();
    }
    CATCH_LOG();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleScreenBufferSizeImpl(SCREEN_INFORMATION& context,
                                                    const COORD size) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        SCREEN_INFORMATION& screenInfo = context.GetActiveBuffer();

        // see MSFT:17415266
        // We only really care about the minimum window size if we have a head.
        if (!ServiceLocator::LocateGlobals().IsHeadless())
        {
            COORD const coordMin = screenInfo.GetMinWindowSizeInCharacters();
            // Make sure requested screen buffer size isn't smaller than the window.
            RETURN_HR_IF(E_INVALIDARG, (size.X < screenInfo.GetViewport().Width() ||
                                        size.Y < screenInfo.GetViewport().Height() ||
                                        size.Y < coordMin.Y ||
                                        size.X < coordMin.X));
        }

        // Ensure the requested size isn't larger than we can handle in our data type.
        RETURN_HR_IF(E_INVALIDARG, (size.X == SHORT_MAX || size.Y == SHORT_MAX));

        // Only do the resize if we're actually changing one of the dimensions
        COORD const coordScreenBufferSize = screenInfo.GetBufferSize().Dimensions();
        if (size.X != coordScreenBufferSize.X || size.Y != coordScreenBufferSize.Y)
        {
            RETURN_NTSTATUS(screenInfo.ResizeScreenBuffer(size, TRUE));
        }

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleScreenBufferInfoExImpl(SCREEN_INFORMATION& context,
                                                      const CONSOLE_SCREEN_BUFFER_INFOEX& data) noexcept
{
    try
    {
        RETURN_HR_IF(E_INVALIDARG, (data.dwSize.X == 0 ||
                                    data.dwSize.Y == 0 ||
                                    data.dwSize.X == SHRT_MAX ||
                                    data.dwSize.Y == SHRT_MAX));

        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        Globals& g = ServiceLocator::LocateGlobals();
        CONSOLE_INFORMATION& gci = g.getConsoleInformation();

        const COORD coordScreenBufferSize = context.GetBufferSize().Dimensions();
        const COORD requestedBufferSize = data.dwSize;
        if (requestedBufferSize.X != coordScreenBufferSize.X ||
            requestedBufferSize.Y != coordScreenBufferSize.Y)
        {
            CommandLine& commandLine = CommandLine::Instance();

            commandLine.Hide(FALSE);

            LOG_IF_FAILED(context.ResizeScreenBuffer(data.dwSize, TRUE));

            commandLine.Show();
        }
        const COORD newBufferSize = context.GetBufferSize().Dimensions();

        gci.SetColorTable(data.ColorTable, ARRAYSIZE(data.ColorTable));
        SetScreenColors(context, data.wAttributes, data.wPopupAttributes, TRUE, gci.GetDefaultForegroundColor(), gci.GetDefaultBackgroundColor());

        const Viewport requestedViewport = Viewport::FromExclusive(data.srWindow);

        COORD NewSize = requestedViewport.Dimensions();
        // If we have a window, clamp the requested viewport to the max window size
        if (!ServiceLocator::LocateGlobals().IsHeadless())
        {
            NewSize.X = std::min(NewSize.X, data.dwMaximumWindowSize.X);
            NewSize.Y = std::min(NewSize.Y, data.dwMaximumWindowSize.Y);
        }

        // If wrap text is on, then the window width must be the same size as the buffer width
        if (gci.GetWrapText())
        {
            NewSize.X = newBufferSize.X;
        }

        if (NewSize.X != context.GetViewport().Width() ||
            NewSize.Y != context.GetViewport().Height())
        {
            context.SetViewportSize(&NewSize);

            IConsoleWindow* const pWindow = ServiceLocator::LocateConsoleWindow();
            if (pWindow != nullptr)
            {
                pWindow->UpdateWindowSize(NewSize);
            }
        }

        // Despite the fact that this API takes in a srWindow for the viewport, it traditionally actually doesn't set
        //  anything using that member - for moving the viewport, you need SetConsoleWindowInfo
        //  (see https://msdn.microsoft.com/en-us/library/windows/desktop/ms686125(v=vs.85).aspx and DoSrvSetConsoleWindowInfo)
        // Note that it also doesn't set cursor position.

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleCursorPositionImpl(SCREEN_INFORMATION& context,
                                                  const COORD position) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

        const COORD coordScreenBufferSize = context.GetBufferSize().Dimensions();
        RETURN_HR_IF(E_INVALIDARG, (position.X >= coordScreenBufferSize.X ||
                                    position.Y >= coordScreenBufferSize.Y ||
                                    position.X < 0 ||
                                    position.Y < 0));

        // MSFT: 15813316 - Try to use this SetCursorPosition call to inherit the cursor position.
        RETURN_IF_FAILED(gci.GetVtIo()->SetCursorPosition(position));

        RETURN_IF_NTSTATUS_FAILED(context.SetCursorPosition(position, true));

        LOG_IF_FAILED(ConsoleImeResizeCompStrView());

        COORD WindowOrigin;
        WindowOrigin.X = 0;
        WindowOrigin.Y = 0;
        {
            const SMALL_RECT currentViewport = context.GetViewport().ToInclusive();
            if (currentViewport.Left > position.X)
            {
                WindowOrigin.X = position.X - currentViewport.Left;
            }
            else if (currentViewport.Right < position.X)
            {
                WindowOrigin.X = position.X - currentViewport.Right;
            }

            if (currentViewport.Top > position.Y)
            {
                WindowOrigin.Y = position.Y - currentViewport.Top;
            }
            else if (currentViewport.Bottom < position.Y)
            {
                WindowOrigin.Y = position.Y - currentViewport.Bottom;
            }
        }

        RETURN_IF_NTSTATUS_FAILED(context.SetViewportOrigin(false, WindowOrigin, true));

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleCursorInfoImpl(SCREEN_INFORMATION& context,
                                              const ULONG size,
                                              const bool isVisible) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        // If more than 100% or less than 0% cursor height, reject it.
        RETURN_HR_IF(E_INVALIDARG, (size > 100 || size == 0));

        context.SetCursorInformation(size,
                                     isVisible,
                                     context.GetTextBuffer().GetCursor().GetColor(),
                                     context.GetTextBuffer().GetCursor().GetType());

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleWindowInfoImpl(SCREEN_INFORMATION& context,
                                              const bool isAbsolute,
                                              const SMALL_RECT& windowRect) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        Globals& g = ServiceLocator::LocateGlobals();
        SMALL_RECT Window = windowRect;

        if (!isAbsolute)
        {
            SMALL_RECT currentViewport = context.GetViewport().ToInclusive();
            Window.Left += currentViewport.Left;
            Window.Right += currentViewport.Right;
            Window.Top += currentViewport.Top;
            Window.Bottom += currentViewport.Bottom;
        }

        RETURN_HR_IF(E_INVALIDARG, (Window.Right < Window.Left || Window.Bottom < Window.Top));

        COORD NewWindowSize;
        NewWindowSize.X = (SHORT)(CalcWindowSizeX(Window));
        NewWindowSize.Y = (SHORT)(CalcWindowSizeY(Window));

        // see MSFT:17415266
        // If we have a actual head, we care about the maximum size the window can be.
        // if we're headless, not so much. However, GetMaxWindowSizeInCharacters
        //      will only return the buffer size, so we can't use that to clip the arg here.
        // So only clip the requested size if we're not headless
        if (!g.IsHeadless())
        {
            COORD const coordMax = context.GetMaxWindowSizeInCharacters();
            RETURN_HR_IF(E_INVALIDARG, (NewWindowSize.X > coordMax.X || NewWindowSize.Y > coordMax.Y));

        }
        else if (g.getConsoleInformation().IsInVtIoMode())
        {
            // SetViewportRect doesn't cause the buffer to resize. Manually resize the buffer.
            RETURN_IF_NTSTATUS_FAILED(context.ResizeScreenBuffer(Viewport::FromInclusive(Window).Dimensions(), false));
        }

        // Even if it's the same size, we need to post an update in case the scroll bars need to go away.
        context.SetViewport(Viewport::FromInclusive(Window));
        if (context.IsActiveScreenBuffer())
        {
            // TODO: MSFT: 9574827 - shouldn't we be looking at or at least logging the failure codes here? (Or making them non-void?)
            context.PostUpdateWindowSize();
            WriteToScreen(context, context.GetViewport());
        }
        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::ScrollConsoleScreenBufferAImpl(SCREEN_INFORMATION& Context,
                                                    const SMALL_RECT* const pSourceRectangle,
                                                    const COORD* const pTargetOrigin,
                                                    _In_opt_ const SMALL_RECT* const pTargetClipRectangle,
                                                    const char chFill,
                                                    const WORD attrFill)
{
    wchar_t const wchFill = CharToWchar(&chFill, 1);

    return ScrollConsoleScreenBufferWImpl(Context,
                                          pSourceRectangle,
                                          pTargetOrigin,
                                          pTargetClipRectangle,
                                          wchFill,
                                          attrFill);
}


[[nodiscard]]
HRESULT ApiRoutines::ScrollConsoleScreenBufferWImpl(SCREEN_INFORMATION& Context,
                                                    const SMALL_RECT* const pSourceRectangle,
                                                    const COORD* const pTargetOrigin,
                                                    _In_opt_ const SMALL_RECT* const pTargetClipRectangle,
                                                    const wchar_t wchFill,
                                                    const WORD attrFill)
{
    LockConsole();
    auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

    return DoSrvScrollConsoleScreenBufferW(Context,
                                           pSourceRectangle,
                                           pTargetOrigin,
                                           pTargetClipRectangle,
                                           wchFill,
                                           attrFill);
}

[[nodiscard]]
HRESULT DoSrvScrollConsoleScreenBufferW(SCREEN_INFORMATION& screenInfo,
                                        const SMALL_RECT* const pSourceRectangle,
                                        const COORD* const pTargetOrigin,
                                        _In_opt_ const SMALL_RECT* const pTargetClipRectangle,
                                        const wchar_t wchFill,
                                        const WORD attrFill)
{
    CHAR_INFO Fill;
    Fill.Char.UnicodeChar = wchFill;
    Fill.Attributes = attrFill;

    try
    {
        ScrollRegion(screenInfo,
                     *pSourceRectangle,
                     pTargetClipRectangle == nullptr ? std::nullopt : std::optional<SMALL_RECT>(*pTargetClipRectangle),
                     *pTargetOrigin,
                     Fill);
    }
    CATCH_RETURN();

    return S_OK;
}

void SetScreenColors(SCREEN_INFORMATION& screenInfo,
                     const WORD Attributes,
                     const WORD PopupAttributes,
                     const BOOL UpdateWholeScreen,
                     const COLORREF defaultForeground,
                     const COLORREF defaultBackground)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    const TextAttribute oldPrimaryAttributes = screenInfo.GetAttributes();
    const TextAttribute oldPopupAttributes = *screenInfo.GetPopupAttributes();
    const WORD DefaultAttributes = oldPrimaryAttributes.GetLegacyAttributes();
    const WORD DefaultPopupAttributes = oldPopupAttributes.GetLegacyAttributes();

    const TextAttribute NewPrimaryAttributes = TextAttribute(Attributes);
    const TextAttribute NewPopupAttributes = TextAttribute(PopupAttributes);

    // See MSFT: 16709105
    // If we're updating the whole screen, we're also changing the value of the
    //      default attributes. This can happen when we change properties with
    //      the menu, or set the colors with SetConsoleScreenBufferinfoEx. When
    //      that happens, we want to update the default attributes in the VT
    //      adapter as well. If we're not updating the screen (like in a call to
    //      SetConsoleTextAttributes), then we only want to change the currently
    //      active attributes, but the adapter's notion of the "default" should
    //      be left untouched.
    if (UpdateWholeScreen)
    {
        screenInfo.SetDefaultAttributes(NewPrimaryAttributes, NewPopupAttributes);

        // Replace existing defaults with new defaults.
        TextAttribute newDefaults = NewPrimaryAttributes;
        newDefaults.SetDefaultForeground(defaultForeground, Attributes);
        newDefaults.SetDefaultBackground(defaultBackground, Attributes);

        screenInfo.ReplaceDefaultAttributes(oldPrimaryAttributes,
                                            oldPopupAttributes,
                                            newDefaults,
                                            NewPopupAttributes);

        auto& commandLine = CommandLine::Instance();
        if (commandLine.HasPopup())
        {
            commandLine.UpdatePopups(Attributes, PopupAttributes, DefaultAttributes, DefaultPopupAttributes);
        }

        // force repaint of entire line
        WriteToScreen(screenInfo, screenInfo.GetViewport());
    }
    else
    {
        screenInfo.SetAttributes(NewPrimaryAttributes);
        screenInfo.SetPopupAttributes(NewPopupAttributes);

    }
    gci.ConsoleIme.RefreshAreaAttributes();

}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleTextAttributeImpl(SCREEN_INFORMATION& context,
                                                 const WORD attribute) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(attribute, ~VALID_TEXT_ATTRIBUTES));
        const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

        SetScreenColors(context,
                        attribute,
                        context.GetPopupAttributes()->GetLegacyAttributes(),
                        FALSE,
                        gci.GetDefaultForegroundColor(),
                        gci.GetDefaultBackgroundColor());
        return S_OK;
    }
    CATCH_RETURN();
}

void DoSrvPrivateSetLegacyAttributes(SCREEN_INFORMATION& screenInfo,
                                     const WORD Attribute,
                                     const bool fForeground,
                                     const bool fBackground,
                                     const bool fMeta)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    const TextAttribute OldAttributes = screenInfo.GetAttributes();
    TextAttribute NewAttributes = OldAttributes;

    // Always update the legacy component. This prevents the 1m in "^[[32m^[[1m"
    //  from resetting the colors set by the 32m. (for example)
    WORD wNewLegacy = NewAttributes.GetLegacyAttributes();
    if (fForeground)
    {
        WI_UpdateFlagsInMask(wNewLegacy, FG_ATTRS, Attribute);
    }
    if (fBackground)
    {
        WI_UpdateFlagsInMask(wNewLegacy, BG_ATTRS, Attribute);
    }
    if (fMeta)
    {
        WI_UpdateFlagsInMask(wNewLegacy, META_ATTRS, Attribute);
    }
    NewAttributes.SetFromLegacy(wNewLegacy);

    if (!OldAttributes.IsLegacy())
    {
        // The previous call to SetFromLegacy is going to trash our RGB.
        // Restore it.
        // If the old attributes were "reversed", and the new ones aren't,
        //  then flip the colors back.
        bool resetReverse = fMeta &&
            (WI_IsFlagClear(Attribute, COMMON_LVB_REVERSE_VIDEO)) &&
            (WI_IsFlagSet(OldAttributes.GetLegacyAttributes(), COMMON_LVB_REVERSE_VIDEO));

        if (OldAttributes.ForegroundIsDefault() && !fForeground)
        {
            NewAttributes.SetDefaultForeground(gci.GetDefaultForeground(), gci.GetFillAttribute());
        }
        if (OldAttributes.BackgroundIsDefault() && !fBackground)
        {
            NewAttributes.SetDefaultBackground(gci.GetDefaultBackground(), gci.GetFillAttribute());
        }

        if (resetReverse)
        {
            NewAttributes.SetForeground(OldAttributes.CalculateRgbBackground());
            NewAttributes.SetBackground(OldAttributes.CalculateRgbForeground());
        }
        else
        {
            NewAttributes.SetForeground(OldAttributes.CalculateRgbForeground());
            NewAttributes.SetBackground(OldAttributes.CalculateRgbBackground());
        }

        if (fForeground)
        {
            COLORREF rgbColor = gci.GetColorTableEntry((Attribute & FG_ATTRS) | (NewAttributes.IsBold() ? FOREGROUND_INTENSITY : 0));
            NewAttributes.SetForeground(rgbColor);
        }
        if (fBackground)
        {
            COLORREF rgbColor = gci.GetColorTableEntry((Attribute >> 4) & FG_ATTRS);
            NewAttributes.SetBackground(rgbColor);
        }
        if (fMeta)
        {
            NewAttributes.SetMetaAttributes(Attribute);
        }
    }

    screenInfo.SetAttributes(NewAttributes);
}

void DoSrvPrivateSetDefaultAttributes(SCREEN_INFORMATION& screenInfo,
                                      const bool fForeground,
                                      const bool fBackground)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    const auto wFill = gci.GetFillAttribute();
    const auto defaultFG = gci.GetDefaultForeground();
    const auto defaultBG = gci.GetDefaultBackground();

    TextAttribute NewAttributes = screenInfo.GetAttributes();
    if (fForeground)
    {
        if (defaultFG != INVALID_COLOR)
        {
            NewAttributes.SetDefaultForeground(defaultFG, wFill);
            screenInfo.SetAttributes(NewAttributes);
        }
        else
        {
            DoSrvPrivateSetLegacyAttributes(screenInfo, wFill, true, false, false);
        }
    }
    NewAttributes = screenInfo.GetAttributes();
    if (fBackground)
    {
        if (defaultBG != INVALID_COLOR)
        {
            NewAttributes.SetDefaultBackground(defaultBG, wFill);
            screenInfo.SetAttributes(NewAttributes);
        }
        else
        {
            DoSrvPrivateSetLegacyAttributes(screenInfo, wFill, false, true, false);
        }
    }

}

void DoSrvPrivateSetConsoleXtermTextAttribute(SCREEN_INFORMATION& screenInfo,
                                              const int iXtermTableEntry,
                                              const bool fIsForeground)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    TextAttribute NewAttributes = screenInfo.GetAttributes();

    COLORREF rgbColor;
    if (iXtermTableEntry < COLOR_TABLE_SIZE)
    {
        //Convert the xterm index to the win index
        WORD iWinEntry = ::XtermToWindowsIndex(iXtermTableEntry);

        rgbColor = gci.GetColorTableEntry(iWinEntry);
    }
    else
    {
        rgbColor = gci.GetColorTableEntry(iXtermTableEntry);
    }

    NewAttributes.SetColor(rgbColor, fIsForeground);

    screenInfo.SetAttributes(NewAttributes);
}

void DoSrvPrivateSetConsoleRGBTextAttribute(SCREEN_INFORMATION& screenInfo,
                                            const COLORREF rgbColor,
                                            const bool fIsForeground)
{
    TextAttribute NewAttributes = screenInfo.GetAttributes();
    NewAttributes.SetColor(rgbColor, fIsForeground);
    screenInfo.SetAttributes(NewAttributes);
}

void DoSrvPrivateBoldText(SCREEN_INFORMATION& screenInfo, const bool bolded)
{
    auto attrs = screenInfo.GetAttributes();
    if (bolded)
    {
        attrs.Embolden();
    }
    else
    {
        attrs.Debolden();
    }
    screenInfo.SetAttributes(attrs);
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleOutputCodePageImpl(const ULONG codepage) noexcept
{
    try
    {
        CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        // Return if it's not known as a valid codepage ID.
        RETURN_HR_IF(E_INVALIDARG, !(IsValidCodePage(codepage)));

        // Do nothing if no change.
        if (gci.OutputCP != codepage)
        {
            // Set new code page
            gci.OutputCP = codepage;

            SetConsoleCPInfo(TRUE);
        }

        return S_OK;
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleInputCodePageImpl(const ULONG codepage) noexcept
{
    try
    {
        CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        // Return if it's not known as a valid codepage ID.
        RETURN_HR_IF(E_INVALIDARG, !(IsValidCodePage(codepage)));

        // Do nothing if no change.
        if (gci.CP != codepage)
        {
            // Set new code page
            gci.CP = codepage;

            SetConsoleCPInfo(FALSE);
        }

        return S_OK;
    }
    CATCH_RETURN();
}

void ApiRoutines::GetConsoleInputCodePageImpl(ULONG& codepage) noexcept
{
    try
    {
        const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        codepage = gci.CP;
    }
    CATCH_LOG();
}

void DoSrvGetConsoleOutputCodePage(_Out_ unsigned int* const pCodePage)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    *pCodePage = gci.OutputCP;
}

void ApiRoutines::GetConsoleOutputCodePageImpl(ULONG& codepage) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });
        unsigned int uiCodepage;
        DoSrvGetConsoleOutputCodePage(&uiCodepage);
        codepage = uiCodepage;
    }
    CATCH_LOG();
}

void ApiRoutines::GetConsoleWindowImpl(HWND& hwnd) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });
        const IConsoleWindow* pWindow = ServiceLocator::LocateConsoleWindow();
        const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        if (pWindow != nullptr)
        {
            hwnd = pWindow->GetWindowHandle();
        }
        else
        {
            // Some applications will fail silently if this API returns 0 (cygwin)
            // If we're in pty mode, we need to return a fake window handle that
            //      doesn't actually do anything, but is a unique HWND to this
            //      console, so that they know that this console is in fact a real
            //      console window.
            if (gci.IsInVtIoMode())
            {
                hwnd = ServiceLocator::LocatePseudoWindow();
            }
        }
    }
    CATCH_LOG();
}

void ApiRoutines::GetConsoleHistoryInfoImpl(CONSOLE_HISTORY_INFO& consoleHistoryInfo) noexcept
{
    try
    {
        const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        consoleHistoryInfo.HistoryBufferSize = gci.GetHistoryBufferSize();
        consoleHistoryInfo.NumberOfHistoryBuffers = gci.GetNumberOfHistoryBuffers();
        WI_SetFlagIf(consoleHistoryInfo.dwFlags, HISTORY_NO_DUP_FLAG, WI_IsFlagSet(gci.Flags, CONSOLE_HISTORY_NODUP));
    }
    CATCH_LOG();
}

HRESULT ApiRoutines::SetConsoleHistoryInfoImpl(const CONSOLE_HISTORY_INFO& consoleHistoryInfo) noexcept
{
    try
    {
        CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
        RETURN_HR_IF(E_INVALIDARG, consoleHistoryInfo.HistoryBufferSize > SHORT_MAX);
        RETURN_HR_IF(E_INVALIDARG, consoleHistoryInfo.NumberOfHistoryBuffers > SHORT_MAX);
        RETURN_HR_IF(E_INVALIDARG, WI_IsAnyFlagSet(consoleHistoryInfo.dwFlags, ~CHI_VALID_FLAGS));

        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        CommandHistory::s_ResizeAll(consoleHistoryInfo.HistoryBufferSize);
        gci.SetNumberOfHistoryBuffers(consoleHistoryInfo.NumberOfHistoryBuffers);

        WI_UpdateFlag(gci.Flags, CONSOLE_HISTORY_NODUP, WI_IsFlagSet(consoleHistoryInfo.dwFlags, HISTORY_NO_DUP_FLAG));

        return S_OK;
    }
    CATCH_RETURN();
}

// NOTE: This was in private.c, but turns out to be a public API: http://msdn.microsoft.com/en-us/library/windows/desktop/ms683164(v=vs.85).aspx
void ApiRoutines::GetConsoleDisplayModeImpl(ULONG& flags) noexcept
{
    try
    {
        LockConsole();
        auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

        // Initialize flags portion of structure
        flags = 0;

        IConsoleWindow* const pWindow = ServiceLocator::LocateConsoleWindow();
        if (pWindow != nullptr && pWindow->IsInFullscreen())
        {
            WI_SetFlag(flags, CONSOLE_FULLSCREEN_MODE);
        }
    }
    CATCH_LOG();
}

// Routine Description:
// - This routine sets the console display mode for an output buffer.
// - This API is only supported on x86 machines.
// Parameters:
// - context - Supplies a console output handle.
// - flags - Specifies the display mode. Options are:
//      CONSOLE_FULLSCREEN_MODE - data is displayed fullscreen
//      CONSOLE_WINDOWED_MODE - data is displayed in a window
// - newSize - On output, contains the new dimensions of the screen buffer.  The dimensions are in rows and columns for textmode screen buffers.
// Return value:
// - TRUE - The operation was successful.
// - FALSE/nullptr - The operation failed. Extended error status is available using GetLastError.
// NOTE:
// - This was in private.c, but turns out to be a public API:
// - See: http://msdn.microsoft.com/en-us/library/windows/desktop/ms686028(v=vs.85).aspx
[[nodiscard]]
HRESULT ApiRoutines::SetConsoleDisplayModeImpl(SCREEN_INFORMATION& context,
                                               const ULONG flags,
                                               COORD& newSize) noexcept
{
    try
    {
        // SetIsFullscreen() below ultimately calls SetwindowLong, which ultimately calls SendMessage(). If we retain
        // the console lock, we'll deadlock since ConsoleWindowProc takes the lock before processing messages. Instead,
        // we'll release early.
        LockConsole();
        {
            auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

            SCREEN_INFORMATION&  screenInfo = context.GetActiveBuffer();

            newSize = screenInfo.GetBufferSize().Dimensions();
            RETURN_HR_IF(E_INVALIDARG, !(screenInfo.IsActiveScreenBuffer()));
        }

        IConsoleWindow* const pWindow = ServiceLocator::LocateConsoleWindow();
        if (WI_IsFlagSet(flags, CONSOLE_FULLSCREEN_MODE))
        {
            if (pWindow != nullptr)
            {
                pWindow->SetIsFullscreen(true);
            }
        }
        else if (WI_IsFlagSet(flags, CONSOLE_WINDOWED_MODE))
        {
            if (pWindow != nullptr)
            {
                pWindow->SetIsFullscreen(false);
            }
        }
        else
        {
            RETURN_HR(E_INVALIDARG);
        }

        return S_OK;
    }
    CATCH_RETURN();
}

// Routine Description:
// - A private API call for changing the cursor keys input mode between normal and application mode.
//     The cursor keys are the arrows, plus Home and End.
// Parameters:
// - fApplicationMode - set to true to enable Application Mode Input, false for Numeric Mode Input.
// Return value:
// - True if handled successfully. False otherwise.
[[nodiscard]]
NTSTATUS DoSrvPrivateSetCursorKeysMode(_In_ bool fApplicationMode)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    if (gci.pInputBuffer == nullptr)
    {
        return STATUS_UNSUCCESSFUL;
    }
    gci.pInputBuffer->GetTerminalInput().ChangeCursorKeysMode(fApplicationMode);
    return STATUS_SUCCESS;
}

// Routine Description:
// - A private API call for changing the keypad input mode between numeric and application mode.
//     This controls what the keys on the numpad translate to.
// Parameters:
// - fApplicationMode - set to true to enable Application Mode Input, false for Numeric Mode Input.
// Return value:
// - True if handled successfully. False otherwise.
[[nodiscard]]
NTSTATUS DoSrvPrivateSetKeypadMode(_In_ bool fApplicationMode)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    if (gci.pInputBuffer == nullptr)
    {
        return STATUS_UNSUCCESSFUL;
    }
    gci.pInputBuffer->GetTerminalInput().ChangeKeypadMode(fApplicationMode);
    return STATUS_SUCCESS;
}

// Routine Description:
// - A private API call for enabling or disabling the cursor blinking.
// Parameters:
// - fEnable - set to true to enable blinking, false to disable
// Return value:
// - True if handled successfully. False otherwise.
void DoSrvPrivateAllowCursorBlinking(SCREEN_INFORMATION& screenInfo, const bool fEnable)
{
    screenInfo.GetTextBuffer().GetCursor().SetBlinkingAllowed(fEnable);
    screenInfo.GetTextBuffer().GetCursor().SetIsOn(!fEnable);
}

// Routine Description:
// - A private API call for setting the top and bottom scrolling margins for
//     the current page. This creates a subsection of the screen that scrolls
//     when input reaches the end of the region, leaving the rest of the screen
//     untouched.
//  Currently only accessible through the use of ANSI sequence DECSTBM
// Parameters:
// - psrScrollMargins - A rect who's Top and Bottom members will be used to set
//     the new values of the top and bottom margins. If (0,0), then the margins
//     will be disabled. NOTE: This is a rect in the case that we'll need the
//     left and right margins in the future.
// Return value:
// - True if handled successfully. False otherwise.
[[nodiscard]]
NTSTATUS DoSrvPrivateSetScrollingRegion(SCREEN_INFORMATION& screenInfo, const SMALL_RECT* const psrScrollMargins)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (psrScrollMargins->Top > psrScrollMargins->Bottom)
    {
        Status = STATUS_INVALID_PARAMETER;
    }
    if (NT_SUCCESS(Status))
    {
        SMALL_RECT srScrollMargins = screenInfo.GetRelativeScrollMargins().ToInclusive();
        srScrollMargins.Top = psrScrollMargins->Top;
        srScrollMargins.Bottom = psrScrollMargins->Bottom;
        screenInfo.SetScrollMargins(Viewport::FromInclusive(srScrollMargins));
    }

    return Status;
}

// Routine Description:
// - A private API call for performing a "Reverse line feed", essentially, the opposite of '\n'.
//    Moves the cursor up one line, and tries to keep its position in the line
// Parameters:
// - screenInfo - a pointer to the screen buffer that should perform the reverse line feed
// Return value:
// - True if handled successfully. False otherwise.
[[nodiscard]]
NTSTATUS DoSrvPrivateReverseLineFeed(SCREEN_INFORMATION& screenInfo)
{
    NTSTATUS Status = STATUS_SUCCESS;

    const SMALL_RECT viewport = screenInfo.GetViewport().ToInclusive();
    const COORD oldCursorPosition = screenInfo.GetTextBuffer().GetCursor().GetPosition();
    const COORD newCursorPosition = { oldCursorPosition.X, oldCursorPosition.Y - 1 };

    // If the cursor is at the top of the viewport, we don't want to shift the viewport up.
    // We want it to stay exactly where it is.
    // In that case, shift the buffer contents down, to emulate inserting a line
    //      at the top of the buffer.
    if (oldCursorPosition.Y > viewport.Top)
    {
        // Cursor is below the top line of the viewport
        Status = AdjustCursorPosition(screenInfo, newCursorPosition, TRUE, nullptr);
    }
    else
    {
        const auto margins = screenInfo.GetAbsoluteScrollMargins();
        const bool marginsSet = margins.BottomInclusive() > margins.Top();

        // If we don't have margins, or the cursor is within the boundaries of the margins
        // It's important to check if the cursor is in the margins,
        //      If it's not, but the margins are set, then we don't want to scroll anything
        if (!marginsSet || margins.IsInBounds(oldCursorPosition))
        {
            // Cursor is at the top of the viewport
            const COORD bufferSize = screenInfo.GetBufferSize().Dimensions();
            // Rectangle to cut out of the existing buffer
            SMALL_RECT srScroll;
            srScroll.Left = 0;
            srScroll.Right = bufferSize.X;
            srScroll.Top = viewport.Top;
            srScroll.Bottom = viewport.Bottom - 1;
            // Paste coordinate for cut text above
            COORD coordDestination;
            coordDestination.X = 0;
            coordDestination.Y = viewport.Top + 1;

            SMALL_RECT srClip = viewport;

            Status = DoSrvScrollConsoleScreenBufferW(screenInfo, &srScroll, &coordDestination, &srClip, L' ', screenInfo.GetAttributes().GetLegacyAttributes());
        }
    }
    return Status;
}

// Routine Description:
// - A private API call for moving the cursor vertically in the buffer. This is
//      because the vertical cursor movements in VT are constrained by the
//      scroll margins, while the absolute positioning is not.
// Parameters:
// - screenInfo - a reference to the screen buffer we should move the cursor for
// - lines - The number of lines to move the cursor. Up is negative, down positive.
// Return value:
// - True if handled successfully. False otherwise.
[[nodiscard]]
NTSTATUS DoSrvMoveCursorVertically(SCREEN_INFORMATION& screenInfo, const short lines)
{
    NTSTATUS Status = STATUS_SUCCESS;

    Cursor& cursor = screenInfo.GetTextBuffer().GetCursor();
    const int iCurrentCursorY = cursor.GetPosition().Y;
    SMALL_RECT srMargins = screenInfo.GetAbsoluteScrollMargins().ToInclusive();
    const bool fMarginsSet = srMargins.Bottom > srMargins.Top;
    const bool fCursorInMargins = iCurrentCursorY <= srMargins.Bottom && iCurrentCursorY >= srMargins.Top;
    COORD clampedPos = { cursor.GetPosition().X, cursor.GetPosition().Y + lines };

    // Make sure the cursor doesn't move outside the viewport.
    screenInfo.GetViewport().Clamp(clampedPos);

    // Make sure the cursor stays inside the margins
    if (fMarginsSet)
    {
        auto v = clampedPos.Y;
        auto lo = srMargins.Top;
        auto hi = srMargins.Bottom;
        clampedPos.Y = std::clamp(v, lo, hi);
    }
    cursor.SetPosition(clampedPos);

    return Status;
}

// Routine Description:
// - A private API call for swaping to the alternate screen buffer. In virtual terminals, there exists both a "main"
//     screen buffer and an alternate. ASBSET creates a new alternate, and switches to it. If there is an already
//     existing alternate, it is discarded.
// Parameters:
// - screenInfo - a reference to the screen buffer that should use an alternate buffer
// Return value:
// - True if handled successfully. False otherwise.
[[nodiscard]]
NTSTATUS DoSrvPrivateUseAlternateScreenBuffer(SCREEN_INFORMATION& screenInfo)
{
    return screenInfo.UseAlternateScreenBuffer();
}

// Routine Description:
// - A private API call for swaping to the main screen buffer. From the
//     alternate buffer, returns to the main screen buffer. From the main
//     screen buffer, does nothing. The alternate is discarded.
// Parameters:
// - screenInfo - a reference to the screen buffer that should use the main buffer
// Return value:
// - True if handled successfully. False otherwise.
void DoSrvPrivateUseMainScreenBuffer(SCREEN_INFORMATION& screenInfo)
{
    screenInfo.UseMainScreenBuffer();
}

// Routine Description:
// - A private API call for setting a VT tab stop in the cursor's current column.
// Parameters:
// <none>
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
[[nodiscard]]
NTSTATUS DoSrvPrivateHorizontalTabSet()
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    SCREEN_INFORMATION& _screenBuffer = gci.GetActiveOutputBuffer();

    const COORD cursorPos = _screenBuffer.GetTextBuffer().GetCursor().GetPosition();
    try
    {
        _screenBuffer.AddTabStop(cursorPos.X);
    }
    catch (...)
    {
        return NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
    }
    return STATUS_SUCCESS;
}

// Routine Description:
// - A private helper for excecuting a number of tabs.
// Parameters:
// sNumTabs - The number of tabs to execute
// fForward - whether to tab forward or backwards
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
[[nodiscard]]
NTSTATUS DoPrivateTabHelper(const SHORT sNumTabs, _In_ bool fForward)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    SCREEN_INFORMATION& _screenBuffer = gci.GetActiveOutputBuffer();

    NTSTATUS Status = STATUS_SUCCESS;
    FAIL_FAST_IF(!(sNumTabs >= 0));
    for (SHORT sTabsExecuted = 0; sTabsExecuted < sNumTabs && NT_SUCCESS(Status); sTabsExecuted++)
    {
        const COORD cursorPos = _screenBuffer.GetTextBuffer().GetCursor().GetPosition();
        COORD cNewPos = (fForward) ? _screenBuffer.GetForwardTab(cursorPos) : _screenBuffer.GetReverseTab(cursorPos);
        // GetForwardTab is smart enough to move the cursor to the next line if
        // it's at the end of the current one already. AdjustCursorPos shouldn't
        // to be doing anything funny, just moving the cursor to the location GetForwardTab returns
        Status = AdjustCursorPosition(_screenBuffer, cNewPos, TRUE, nullptr);
    }
    return Status;
}

// Routine Description:
// - A private API call for performing a forwards tab. This will take the
//     cursor to the tab stop following its current location. If there are no
//     more tabs in this row, it will take it to the right side of the window.
//     If it's already in the last column of the row, it will move it to the next line.
// Parameters:
// - sNumTabs - The number of tabs to perform.
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
[[nodiscard]]
NTSTATUS DoSrvPrivateForwardTab(const SHORT sNumTabs)
{
    return DoPrivateTabHelper(sNumTabs, true);
}

// Routine Description:
// - A private API call for performing a backwards tab. This will take the
//     cursor to the tab stop previous to its current location. It will not reverse line feed.
// Parameters:
// - sNumTabs - The number of tabs to perform.
// Return value:
// - STATUS_SUCCESS if handled successfully. Otherwise, an approriate status code indicating the error.
[[nodiscard]]
NTSTATUS DoSrvPrivateBackwardsTab(const SHORT sNumTabs)
{
    return DoPrivateTabHelper(sNumTabs, false);
}

// Routine Description:
// - A private API call for clearing the VT tabs that have been set.
// Parameters:
// - fClearAll - If false, only clears the tab in the current column (if it exists)
//      otherwise clears all set tabs. (and reverts to lecacy 8-char tabs behavior.)
// Return value:
// - None
void DoSrvPrivateTabClear(const bool fClearAll)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    SCREEN_INFORMATION& screenBuffer = gci.GetActiveOutputBuffer();
    if (fClearAll)
    {
        screenBuffer.ClearTabStops();
    }
    else
    {
        const COORD cursorPos = screenBuffer.GetTextBuffer().GetCursor().GetPosition();
        screenBuffer.ClearTabStop(cursorPos.X);
    }
}

// Routine Description:
// - A private API call for enabling VT200 style mouse mode.
// Parameters:
// - fEnable - true to enable default tracking mode, false to disable mouse mode.
// Return value:
// - None
void DoSrvPrivateEnableVT200MouseMode(const bool fEnable)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.terminalMouseInput.EnableDefaultTracking(fEnable);
}

// Routine Description:
// - A private API call for enabling utf8 style mouse mode.
// Parameters:
// - fEnable - true to enable, false to disable.
// Return value:
// - None
void DoSrvPrivateEnableUTF8ExtendedMouseMode(const bool fEnable)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.terminalMouseInput.SetUtf8ExtendedMode(fEnable);
}

// Routine Description:
// - A private API call for enabling SGR style mouse mode.
// Parameters:
// - fEnable - true to enable, false to disable.
// Return value:
// - None
void DoSrvPrivateEnableSGRExtendedMouseMode(const bool fEnable)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.terminalMouseInput.SetSGRExtendedMode(fEnable);
}

// Routine Description:
// - A private API call for enabling button-event mouse mode.
// Parameters:
// - fEnable - true to enable button-event mode, false to disable mouse mode.
// Return value:
// - None
void DoSrvPrivateEnableButtonEventMouseMode(const bool fEnable)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.terminalMouseInput.EnableButtonEventTracking(fEnable);
}

// Routine Description:
// - A private API call for enabling any-event mouse mode.
// Parameters:
// - fEnable - true to enable any-event mode, false to disable mouse mode.
// Return value:
// - None
void DoSrvPrivateEnableAnyEventMouseMode(const bool fEnable)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.terminalMouseInput.EnableAnyEventTracking(fEnable);
}

// Routine Description:
// - A private API call for enabling alternate scroll mode
// Parameters:
// - fEnable - true to enable alternate scroll mode, false to disable.
// Return value:
// None
void DoSrvPrivateEnableAlternateScroll(const bool fEnable)
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.terminalMouseInput.EnableAlternateScroll(fEnable);
}

// Routine Description:
// - A private API call for performing a VT-style erase all operation on the buffer.
//      See SCREEN_INFORMATION::VtEraseAll's description for details.
// Parameters:
//  The ScreenBuffer to perform the erase on.
// Return value:
// - STATUS_SUCCESS if we succeeded, otherwise the NTSTATUS version of the failure.
[[nodiscard]]
NTSTATUS DoSrvPrivateEraseAll(SCREEN_INFORMATION&  screenInfo)
{
    return NTSTATUS_FROM_HRESULT(screenInfo.GetActiveBuffer().VtEraseAll());
}

void DoSrvSetCursorStyle(SCREEN_INFORMATION& screenInfo,
                         const CursorType cursorType)
{
    screenInfo.GetTextBuffer().GetCursor().SetType(cursorType);
}

void DoSrvSetCursorColor(SCREEN_INFORMATION& screenInfo,
                         const COLORREF cursorColor)
{
    screenInfo.GetTextBuffer().GetCursor().SetColor(cursorColor);
}

// Routine Description:
// - A private API call to get only the default color attributes of the screen buffer.
// - This is used as a performance optimization by the VT adapter in SGR (Set Graphics Rendition) instead
//   of calling for this information through the public API GetConsoleScreenBufferInfoEx which returns a lot
//   of extra unnecessary data and takes a lot of extra processing time.
// Parameters
// - screenInfo - The screen buffer to retrieve default color attributes information from
// - pwAttributes - Pointer to space that will receive color attributes data
// Return Value:
// - STATUS_SUCCESS if we succeeded or STATUS_INVALID_PARAMETER for bad params (nullptr).
[[nodiscard]]
NTSTATUS DoSrvPrivateGetConsoleScreenBufferAttributes(_In_ const SCREEN_INFORMATION& screenInfo, _Out_ WORD* const pwAttributes)
{
    NTSTATUS Status = STATUS_SUCCESS;

    if (pwAttributes == nullptr)
    {
        Status = STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(Status))
    {
        *pwAttributes = screenInfo.GetActiveBuffer().GetAttributes().GetLegacyAttributes();
    }

    return Status;
}

// Routine Description:
// - A private API call for forcing the renderer to repaint the screen. If the
//      input screen buffer is not the active one, then just do nothing. We only
//      want to redraw the screen buffer that requested the repaint, and
//      switching screen buffers will already force a repaint.
// Parameters:
//  The ScreenBuffer to perform the repaint for.
// Return value:
// - None
void DoSrvPrivateRefreshWindow(_In_ const SCREEN_INFORMATION& screenInfo)
{
    Globals& g = ServiceLocator::LocateGlobals();
    if (&screenInfo == &g.getConsoleInformation().GetActiveOutputBuffer())
    {
        g.pRender->TriggerRedrawAll();
    }
}

void GetConsoleTitleWImplHelper(_Out_writes_to_opt_(cchTitleBufferSize, *pcchTitleBufferWritten) wchar_t* const pwsTitleBuffer,
                                const size_t cchTitleBufferSize,
                                _Out_ size_t* const pcchTitleBufferWritten,
                                _Out_ size_t* const pcchTitleBufferNeeded,
                                const bool fIsOriginal)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    // Ensure output variables are initialized.
    *pcchTitleBufferWritten = 0;
    *pcchTitleBufferNeeded = 0;

    if (nullptr != pwsTitleBuffer)
    {
        *pwsTitleBuffer = L'\0';
    }

    // Get the appropriate title and length depending on the mode.
    const wchar_t* pwszTitle;
    size_t cchTitleLength;

    if (fIsOriginal)
    {
        pwszTitle = gci.GetOriginalTitle().c_str();
        cchTitleLength = gci.GetOriginalTitle().length();
    }
    else
    {
        pwszTitle = gci.GetTitle().c_str();
        cchTitleLength = gci.GetTitle().length();
    }

    // Always report how much space we would need.
    *pcchTitleBufferNeeded = cchTitleLength;

    // If we have a pointer to receive the data, then copy it out.
    if (nullptr != pwsTitleBuffer)
    {
        HRESULT const hr = StringCchCopyNW(pwsTitleBuffer, cchTitleBufferSize, pwszTitle, cchTitleLength);

        // Insufficient buffer is allowed. If we return a partial string, that's still OK by historical/compat standards.
        // Just say how much we managed to return.
        if (SUCCEEDED(hr) || STRSAFE_E_INSUFFICIENT_BUFFER == hr)
        {
            *pcchTitleBufferWritten = std::min(cchTitleBufferSize, cchTitleLength);
        }
    }
}

[[nodiscard]]
HRESULT GetConsoleTitleAImplHelper(_Out_writes_to_(cchTitleBufferSize, *pcchTitleBufferWritten) _Always_(_Post_z_) char* const psTitleBuffer,
                                   const size_t cchTitleBufferSize,
                                   _Out_ size_t* const pcchTitleBufferWritten,
                                   _Out_ size_t* const pcchTitleBufferNeeded,
                                   const bool fIsOriginal)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    // Ensure output variables are initialized.
    *pcchTitleBufferWritten = 0;
    *pcchTitleBufferNeeded = 0;

    if (nullptr != psTitleBuffer)
    {
        *psTitleBuffer = '\0';
    }

    try
    {
        // Figure out how big our temporary Unicode buffer must be to get the title.
        size_t cchUnicodeTitleBufferNeeded;
        size_t cchUnicodeTitleBufferWritten;
        GetConsoleTitleWImplHelper(nullptr, 0, &cchUnicodeTitleBufferWritten, &cchUnicodeTitleBufferNeeded, fIsOriginal);

        // If there's nothing to get, then simply return.
        RETURN_HR_IF(S_OK, 0 == cchUnicodeTitleBufferNeeded);

        // Allocate a unicode buffer of the right size.
        size_t const cchUnicodeTitleBufferSize = cchUnicodeTitleBufferNeeded + 1; // add one for null terminator space
        wistd::unique_ptr<wchar_t[]> pwsUnicodeTitleBuffer = wil::make_unique_nothrow<wchar_t[]>(cchUnicodeTitleBufferSize);
        RETURN_IF_NULL_ALLOC(pwsUnicodeTitleBuffer);

        // Retrieve the title in Unicode.
        GetConsoleTitleWImplHelper(pwsUnicodeTitleBuffer.get(),
                                   cchUnicodeTitleBufferSize,
                                   &cchUnicodeTitleBufferWritten,
                                   &cchUnicodeTitleBufferNeeded,
                                   fIsOriginal);

        // Convert result to A
        const auto converted = ConvertToA(gci.CP, { pwsUnicodeTitleBuffer.get(), cchUnicodeTitleBufferWritten });

        // The legacy A behavior is a bit strange. If the buffer given doesn't have enough space to hold
        // the string without null termination (e.g. the title is 9 long, 10 with null. The buffer given isn't >= 9).
        // then do not copy anything back and do not report how much space we need.
        if (cchTitleBufferSize >= converted.size())
        {
            // Say how many characters of buffer we would need to hold the entire result.
            *pcchTitleBufferNeeded = converted.size();

            // Copy safely to output buffer
            HRESULT const hr = StringCchCopyNA(psTitleBuffer,
                                               cchTitleBufferSize,
                                               converted.data(),
                                               converted.size());


            // Insufficient buffer is allowed. If we return a partial string, that's still OK by historical/compat standards.
            // Just say how much we managed to return.
            if (SUCCEEDED(hr) || STRSAFE_E_INSUFFICIENT_BUFFER == hr)
            {
                // And return the size copied (either the size of the buffer or the null terminated length of the string we filled it with.)
                *pcchTitleBufferWritten = std::min(cchTitleBufferSize, converted.size() + 1);

                // Another compatibility fix... If we had exactly the number of bytes needed for an unterminated string,
                // then replace the terminator left behind by StringCchCopyNA with the final character of the title string.
                if (cchTitleBufferSize == converted.size())
                {
                    psTitleBuffer[cchTitleBufferSize - 1] = converted.data()[converted.size() - 1];
                }
            }
        }
        else
        {
            // If we didn't copy anything back and there is space, null terminate the given buffer and return.
            if (cchTitleBufferSize > 0)
            {
                psTitleBuffer[0] = '\0';
                *pcchTitleBufferWritten = 1;
            }
        }

        return S_OK;
    }
    CATCH_RETURN();

}

[[nodiscard]]
HRESULT ApiRoutines::GetConsoleTitleAImpl(_Out_writes_to_(cchTitleBufferSize, *pcchTitleBufferWritten) _Always_(_Post_z_) char* const psTitleBuffer,
                                          const size_t cchTitleBufferSize,
                                          _Out_ size_t* const pcchTitleBufferWritten,
                                          _Out_ size_t* const pcchTitleBufferNeeded)
{
    LockConsole();
    auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

    return GetConsoleTitleAImplHelper(psTitleBuffer,
                                      cchTitleBufferSize,
                                      pcchTitleBufferWritten,
                                      pcchTitleBufferNeeded,
                                      false);
}

void ApiRoutines::GetConsoleTitleWImpl(_Out_writes_to_(cchTitleBufferSize, *pcchTitleBufferWritten) _Always_(_Post_z_) wchar_t* const pwsTitleBuffer,
                                       const size_t cchTitleBufferSize,
                                       _Out_ size_t* const pcchTitleBufferWritten,
                                       _Out_ size_t* const pcchTitleBufferNeeded)
{
    LockConsole();
    auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

    GetConsoleTitleWImplHelper(pwsTitleBuffer,
                               cchTitleBufferSize,
                               pcchTitleBufferWritten,
                               pcchTitleBufferNeeded,
                               false);
}

[[nodiscard]]
HRESULT ApiRoutines::GetConsoleOriginalTitleAImpl(_Out_writes_to_(cchTitleBufferSize, *pcchTitleBufferWritten) _Always_(_Post_z_) char* const psTitleBuffer,
                                                  const size_t cchTitleBufferSize,
                                                  _Out_ size_t* const pcchTitleBufferWritten,
                                                  _Out_ size_t* const pcchTitleBufferNeeded)
{
    LockConsole();
    auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

    return GetConsoleTitleAImplHelper(psTitleBuffer,
                                      cchTitleBufferSize,
                                      pcchTitleBufferWritten,
                                      pcchTitleBufferNeeded,
                                      true);
}

void ApiRoutines::GetConsoleOriginalTitleWImpl(_Out_writes_to_(cchTitleBufferSize, *pcchTitleBufferWritten) _Always_(_Post_z_) wchar_t* const pwsTitleBuffer,
                                               const size_t cchTitleBufferSize,
                                               _Out_ size_t* const pcchTitleBufferWritten,
                                               _Out_ size_t* const pcchTitleBufferNeeded)
{
    LockConsole();
    auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

    GetConsoleTitleWImplHelper(pwsTitleBuffer,
                               cchTitleBufferSize,
                               pcchTitleBufferWritten,
                               pcchTitleBufferNeeded,
                               true);
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleTitleAImpl(const std::string_view title) noexcept
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    try
    {
        const auto titleW = ConvertToW(gci.CP, title);

        return SetConsoleTitleWImpl(titleW);
    }
    CATCH_RETURN();
}

[[nodiscard]]
HRESULT ApiRoutines::SetConsoleTitleWImpl(const std::wstring_view title) noexcept
{
    LockConsole();
    auto Unlock = wil::scope_exit([&] { UnlockConsole(); });

    return DoSrvSetConsoleTitleW(title);
}

[[nodiscard]]
HRESULT DoSrvSetConsoleTitleW(const std::wstring_view title) noexcept
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    // Sanitize the input if we're in pty mode. No control chars - this string
    //      will get emitted back to the TTY in a VT sequence, and we don't want
    //      to embed control characters in that string.
    if (gci.IsInVtIoMode())
    {
        std::wstring sanitized;
        sanitized.reserve(title.size());
        for (size_t i = 0; i < title.size(); i++)
        {
            if (title.at(i) >= UNICODE_SPACE)
            {
                sanitized.push_back(title.at(i));
            }
        }

        gci.SetTitle({ sanitized });
    }
    else
    {
        // SetTitle will trigger the renderer to update the titlebar for us.
        gci.SetTitle(title);
    }

    return S_OK;
}

// Routine Description:
// - A private API call for forcing the VT Renderer to NOT paint the next resize
//      event. This is used by InteractDispatch, to prevent resizes from echoing
//      between terminal and host.
// Parameters:
//  <none>
// Return value:
// - STATUS_SUCCESS if we succeeded, otherwise the NTSTATUS version of the failure.
[[nodiscard]]
NTSTATUS DoSrvPrivateSuppressResizeRepaint()
{
    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    FAIL_FAST_IF(!(gci.IsInVtIoMode()));
    return NTSTATUS_FROM_HRESULT(gci.GetVtIo()->SuppressResizeRepaint());
}

// Routine Description:
// - An API call for checking if the console host is acting as a pty.
// Parameters:
// - isPty: recieves the bool indicating whether or not we're in pty mode.
// Return value:
//  <none>
void DoSrvIsConsolePty(_Out_ bool* const pIsPty)
{
    const CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    *pIsPty = gci.IsInVtIoMode();
}

// Routine Description:
// - a private API call for setting the default tab stops in the active screen buffer.
void DoSrvPrivateSetDefaultTabStops()
{
    ServiceLocator::LocateGlobals().getConsoleInformation().GetActiveOutputBuffer().SetDefaultVtTabStops();
}

// Routine Description:
// - internal logic for adding or removing lines in the active screen buffer
// Parameters:
// - count - the number of lines to modify
// - insert - true if inserting lines, false if deleting lines
void DoSrvPrivateModifyLinesImpl(const unsigned int count, const bool insert)
{
    auto& screenInfo = ServiceLocator::LocateGlobals().getConsoleInformation().GetActiveOutputBuffer();
    auto& textBuffer = screenInfo.GetTextBuffer();
    const auto cursorPosition = textBuffer.GetCursor().GetPosition();
    const auto margins = screenInfo.GetAbsoluteScrollMargins();
    if (margins.IsInBounds(cursorPosition))
    {
        const auto screenEdges = screenInfo.GetBufferSize().ToInclusive();
        // Rectangle to cut out of the existing buffer
        SMALL_RECT srScroll;
        srScroll.Left = 0;
        srScroll.Right = screenEdges.Right - screenEdges.Left;
        srScroll.Top = cursorPosition.Y;
        srScroll.Bottom = screenEdges.Bottom;
        // Paste coordinate for cut text above
        COORD coordDestination;
        coordDestination.X = 0;
        if (insert)
        {
            coordDestination.Y = (cursorPosition.Y) + gsl::narrow<short>(count);
        }
        else
        {
            coordDestination.Y = (cursorPosition.Y) - gsl::narrow<short>(count);
        }

        SMALL_RECT srClip = screenEdges;
        srClip.Top = cursorPosition.Y;

        LOG_IF_FAILED(DoSrvScrollConsoleScreenBufferW(screenInfo,
                                                      &srScroll,
                                                      &coordDestination,
                                                      &srClip,
                                                      UNICODE_SPACE,
                                                      screenInfo.GetAttributes().GetLegacyAttributes()));
    }
}

// Routine Description:
// - a private API call for deleting lines in the active screen buffer.
// Parameters:
// - count - the number of lines to delete
void DoSrvPrivateDeleteLines(const unsigned int count)
{
    DoSrvPrivateModifyLinesImpl(count, false);
}

// Routine Description:
// - a private API call for inserting lines in the active screen buffer.
// Parameters:
// - count - the number of lines to insert
void DoSrvPrivateInsertLines(const unsigned int count)
{
    DoSrvPrivateModifyLinesImpl(count, true);
}

// Method Description:
// - Snaps the screen buffer's viewport to the "virtual bottom", the last place
//the viewport was before the user scrolled it (with the mouse or scrollbar)
// Arguments:
// - screenInfo: the buffer to move the viewport for.
// Return Value:
// - <none>
void DoSrvPrivateMoveToBottom(SCREEN_INFORMATION& screenInfo)
{
    screenInfo.MoveToBottom();
}

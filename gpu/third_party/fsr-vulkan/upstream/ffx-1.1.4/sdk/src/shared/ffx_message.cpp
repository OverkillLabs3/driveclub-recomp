// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2025 Advanced Micro Devices, Inc.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <FidelityFX/host/ffx_message.h>
#include <FidelityFX/host/ffx_util.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // required for OutputDebugString()
#include <cwchar>     // swprintf (MinGW does not pull this in transitively)
#endif                // #ifdef _WIN32

#if defined(_WIN32) && !defined(_MSC_VER)
// MinGW does not declare the MSVC-only swprintf_s; plain swprintf with an
// explicit buffer size is equivalent for these fixed-size formats.
#define FFX_SWPRINTF_S swprintf
#else
#define FFX_SWPRINTF_S swprintf_s
#endif

static ffxMessageCallback s_messageCallback;
static uint32_t s_debugLevel;

// set the printing callback function
void ffxSetPrintMessageCallback(ffxMessageCallback callback, uint32_t debugLevel)
{
    s_messageCallback = callback;
    s_debugLevel = debugLevel;
    return;
}

void ffxPrintMessage(uint32_t type, const wchar_t* message)
{
#ifdef _WIN32
    if (!s_messageCallback) {
        // Format the message string
        wchar_t buffer[512];
        if (type == FFX_MESSAGE_TYPE_ERROR) {
            FFX_SWPRINTF_S(buffer, 512, L"FSR_API_DEBUG_ERROR: %ls\n", message);
        }
        else if (type == FFX_MESSAGE_TYPE_WARNING) {
            FFX_SWPRINTF_S(buffer, 512, L"FSR_API_DEBUG_WARNING: %ls\n", message);
        }
        OutputDebugStringW(buffer);
    } else {
        s_messageCallback(type, message);
    }
#else
    FFX_UNUSED(type);
    FFX_UNUSED(message);
#endif
    return;
}

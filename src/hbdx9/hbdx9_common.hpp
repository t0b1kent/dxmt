/* SPDX-License-Identifier: LGPL-2.1-or-later. HyperBridge DX9, part of the MacRunner fork of DXMT. Copyright (c) 2026 Timur Ravilov. See README-MACRUNNER.md. */ /* HBDX9 thin guest shim — common definitions (PE side, translated code: keep it small). */
#pragma once
#define COBJMACROS
#include <windows.h>
#include <initguid.h> /* single translation unit: define the d3d9 IIDs here */
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>

void hbdx9_unimpl(const char *name);
void hbdx9_log(const char *fmt, ...);

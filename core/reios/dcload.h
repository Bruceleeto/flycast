#pragma once
#include "types.h"
#include <string>

// Trap address of the emulated dcload syscall (the pointer at 0x8c004008 leads here).
constexpr u32 DCLOAD_HOOK_ADDR = 0x8c00400c;

// Pose as dcload when a "pc" folder sits next to the booted ELF, serving /pc/ from it.
void dcload_install(const std::string& elfPath);
// reios trap handler: r4 = command, r5-r7 = arguments, result in r0.
void dcload_syscall();

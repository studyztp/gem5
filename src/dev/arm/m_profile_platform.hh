/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DEV_ARM_M_PROFILE_PLATFORM_HH__
#define __DEV_ARM_M_PROFILE_PLATFORM_HH__

/** @file
 * M-profile (Cortex-M) platform.
 *
 * Minimal C++ backing class for ArmMPlatform.  The real configuration
 * lives in Python (MProfilePlatform.py); this class exists because
 * Platform has pure virtual methods that must be implemented.
 *
 * On A-profile, Platform::postConsoleInt/clearConsoleInt route UART
 * interrupts through the system interrupt controller.  On M-profile,
 * UART interrupts go through the NVIC (via MProfileSCS::sendInt),
 * so these methods are no-ops.
 */

#include "dev/platform.hh"
#include "params/ArmMPlatform.hh"

namespace gem5
{

class ArmMPlatform : public Platform
{
  public:
    PARAMS(ArmMPlatform);
    ArmMPlatform(const Params &p);

    /**
     * No-op: M-profile UART interrupts go through the NVIC
     * (MProfileSCS::sendInt), not through Platform's console
     * interrupt mechanism.
     */
    void postConsoleInt() override {}
    void clearConsoleInt() override {}
};

} // namespace gem5

#endif // __DEV_ARM_M_PROFILE_PLATFORM_HH__

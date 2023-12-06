/*
 * Copyright (c) 2023 The Regents of the University of California.
 * All rights reserved.
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

#include "cpu/probes/global_inst_counter.hh"

namespace gem5
{

GlobalInstCounter::GlobalInstCounter(
    const GlobalInstCounterParams &p)
    : SimObject(p),
    target_inst_count(p.target),
    global_inst_count(0)
{
}

LocalInstCounter::LocalInstCounter(
    const LocalInstCounterParams &p)
    : ProbeListenerObject(p),
    global_counter(p.globalCounter),
    local_counter(0),
    update_threshold(p.updateThreshold),
    if_listen_from_start(p.ifListeningFromStart)
{
}

void
LocalInstCounter::regProbeListeners()
{
    if (if_listen_from_start) {
        listeners.push_back(
            new LocalInstCounterListener(this, "RetiredInstsPC",
                                            &LocalInstCounter::countInst));
    }
}

void
LocalInstCounter::startListening()
{
    if (listeners.empty())
    {
        listeners.push_back(new LooppointAnalysisListener(
                                        this, "RetiredInstsPC",
                                                &LocalInstCounter::countInst));
    }
}

void
LocalInstCounter::stopListening()
{
    for (auto l = listeners.begin(); l != listeners.end(); ++l) {
        delete (*l);
    }
    listeners.clear();
}


} // namespace gem5

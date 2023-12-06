#ifndef __GLOBAL_INST_TRACKER_HH__
#define __GLOBAL_INST_TRACKER_HH__

#include "base/types.hh"
#include "params/GlobalInstCounter.hh"
#include "params/LocalInstCounter.hh"
#include "sim/probe/probe.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

class GlobalInstCounter : public SimObject
{
  private:
    uint64_t target_inst_count;
    uint64_t global_inst_count;

  public:
    GlobalInstCounter(const GlobalInstCounterParams &params);
    void update_global_inst(uint64_t local_inst) {
        global_inst_count += local_inst;
        if (global_inst_count >= target_inst_count) {
            exitSimLoopNow("simpoint starting point found");
        }
    };
    void clearGlobalCount() {
        global_inst_count = 0;
    };
    void updateTargetInst(uint64_t new_target) {
        target_inst_count = new_target;
    };
};

class LocalInstCounter: public ProbeListenerObject
{
  private:
    GlobalInstCounter* global_counter;
    uint64_t local_counter;
    uint64_t update_threshold;
    bool if_listen_from_start;
    typedef ProbeListenerArg<LocalInstCounter, Addr> LocalInstCounterListener;

  public:
    LocalInstCounter(const LocalInstCounterParams &params);
    virtual void regProbeListeners();
    void countInst(const Addr& pc) {
        local_counter += 1;
        if (local_counter >= update_threshold) {
            global_counter->update_global_inst(local_counter);
            local_counter = 0;
        }
    };
    void clearLocalCount() {
        local_counter = 0;
    };
    void updateThreshold(uint64_t new_threshold) {
        update_threshold = new_threshold;
    };
    void startListening();
    void stopListening();
};


} // namespace gem5


#endif // __GLOBAL_INST_TRACKER_HH__

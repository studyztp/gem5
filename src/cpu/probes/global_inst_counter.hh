#ifndef __GLOBAL_INST_COUNTER_HH__
#define __GLOBAL_INST_COUNTER_HH__

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
    void update_global_inst(uint64_t local_inst);
    void clearGlobalCount() {
        global_inst_count = 0;
    };
    void updateTargetInst(uint64_t new_target) {
        target_inst_count = new_target;
    };
    uint64_t current_inst_count() {
        return global_inst_count;
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
    void countInst(const Addr& pc);
    virtual void regProbeListeners();
    void clearLocalCount() {
        local_counter = 0;
    };
    void updateThreshold(uint64_t new_threshold) {
        update_threshold = new_threshold;
    };
    virtual void startListening();
    virtual void stopListening();
    uint64_t current_inst_count() {
        return local_counter;
    };
};


} // namespace gem5


#endif // __GLOBAL_INST_COUNTER_HH__

#ifndef __CPU_PROBES_USER_INST_COUNTER_HH__
#define __CPU_PROBES_USER_INST_COUNTER_HH__

#include "params/UserInstCounter.hh"
#include "params/UserInstCounterManager.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

class UserInstCounterManager : public SimObject
{
  public:
    UserInstCounterManager(const UserInstCounterManagerParams &params);

  private:
    uint64_t userInstCount;
    uint64_t noneUserInstCount;

  public:
    void increaseUserInstCount() { userInstCount++; }
    void increaseNoneUserInstCount() { noneUserInstCount++; }
    uint64_t getUserInstCount() const { return userInstCount; }
    uint64_t getNoneUserInstCount() const { return noneUserInstCount; }
    void resetUserInstCount() { userInstCount = 0; }
    void resetNoneUserInstCount() { noneUserInstCount = 0; }
};

class UserInstCounter : public ProbeListenerObject
{
  public:
    UserInstCounter(const UserInstCounterParams &params);

    bool ifStartListening;
    virtual void regProbeListeners();
    void startListening();
    void stopListening();

    void checkInst(const bool& isUserInst);

  private:
    UserInstCounterManager *manager;
};

} // namespace gem5


#endif // __CPU_PROBES_USER_INST_COUNTER_HH__

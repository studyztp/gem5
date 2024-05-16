#include "cpu/probes/user_inst_counter.hh"

namespace gem5
{
UserInstCounterManager::UserInstCounterManager(
                                const UserInstCounterManagerParams &params) :
    SimObject(params),
    userInstCount(0),
    noneUserInstCount(0)
{
}

UserInstCounter::UserInstCounter(const UserInstCounterParams &params) :
    ProbeListenerObject(params),
    ifStartListening(params.if_start_listening),
    manager(params.counter_manager)
{
    if (params.counter_manager == nullptr){
        fatal("UserInstCounter must have a UserInstCounterManager\n");
    }
}

void
UserInstCounter::regProbeListeners()
{
    if (ifStartListening){
        typedef ProbeListenerArg<UserInstCounter, const bool>
                                                UserInstCounterProbeListener;
        listeners.push_back(new UserInstCounterProbeListener(this,
                            "CommitUserInst", &UserInstCounter::checkInst));
    }
}

void
UserInstCounter::startListening()
{
    if (listeners.size() == 0) {
        typedef ProbeListenerArg<UserInstCounter, const bool> U
                                                serInstCounterProbeListener;
        ifStartListening = true;
        listeners.push_back(new UserInstCounterProbeListener(this,
                            "CommitUserInst", &UserInstCounter::checkInst));
    }

}

void
UserInstCounter::stopListening()
{
    ifStartListening = false;
    listeners.clear();
}

void
UserInstCounter::checkInst(const bool &isUserInst)
{
    if (isUserInst){
        manager->increaseUserInstCount();
    } else {
        manager->increaseNoneUserInstCount();
    }
}

} // namespace gem5

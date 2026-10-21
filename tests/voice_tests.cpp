#include "VoiceMqSim.h"
#include <chrono>
#include <thread>
int main() {
    msad::VoiceMqSim first,second;
    for(int n=0;n<2;++n) {
        if(!first.start() || !second.start()) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::thread a([&]{first.stop();}), b([&]{first.stop();});
        a.join(); b.join(); second.stop();
    }
    return 0;
}

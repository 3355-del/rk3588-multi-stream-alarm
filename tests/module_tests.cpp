#include "MultiStreamEngine.h"
#include <chrono>
#include <thread>
int main() {
    msad::MultiStreamEngine engine;
    std::vector<msad::MultiStreamEngine::StreamConfig> cfg={{42,"mock://stress",128,96,100,30}};
    for(int i=0;i<2;++i) {
        if(!engine.start(cfg,2)) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        engine.stop();
    }
    cfg[0].frameWidth=127;
    if(engine.start(cfg,2)) return 2;
    return 0;
}

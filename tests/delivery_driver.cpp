#include "msad/DurableAlarmNotifier.h"
#include <chrono>
#include <thread>
#include <iostream>
int main(int argc,char** argv) {
 try {
    if(argc!=5) return 2;
    msad::DurableAlarmNotifier notifier(argv[1]);notifier.connect("127.0.0.1",std::stoi(argv[2]));
    for(int i=0;i<std::stoi(argv[3]);++i) { msad::AlarmInfo a;a.streamId=9;notifier.sendAlarm(a); }
    std::cout<<"READY"<<std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(argv[4])));
    notifier.close();return 0;
 } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}

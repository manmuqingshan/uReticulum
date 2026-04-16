#include "rtreticulum/log.h"
#include "rtreticulum/os.h"

int main() {
    using namespace RNS;

    loglevel(LOG_TRACE);

    NOTICE("RTReticulum smoke test starting");
    INFOF("boot millis: %llu", (unsigned long long)Utilities::OS::ltime());
    INFOF("unix seconds: %.3f", Utilities::OS::time());

    Utilities::OS::set_loop_callback([] { TRACE("loop tick"); });

    for (int i = 0; i < 3; ++i) {
        Utilities::OS::run_loop();
        Utilities::OS::sleep(0.1f);
    }

    NOTICE("RTReticulum smoke test done");
    return 0;
}

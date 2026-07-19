#include "EspNowBridge.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<EspNowBridge>();
    return 0;
}

}

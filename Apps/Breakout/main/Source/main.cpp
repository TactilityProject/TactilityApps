#include "Breakout.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<Breakout>();
    return 0;
}

}

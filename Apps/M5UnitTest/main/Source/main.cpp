#include "M5UnitTest.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<M5UnitTest>();
    return 0;
}

}

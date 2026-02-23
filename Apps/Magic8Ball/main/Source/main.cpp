#include "Magic8Ball.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<Magic8Ball>();
    return 0;
}

}

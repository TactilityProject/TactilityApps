#include "Brainfuck.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<Brainfuck>();
    return 0;
}

}

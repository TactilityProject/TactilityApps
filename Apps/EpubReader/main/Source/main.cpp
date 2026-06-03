#include "EpubReader.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<EpubReader>();
    return 0;
}

}

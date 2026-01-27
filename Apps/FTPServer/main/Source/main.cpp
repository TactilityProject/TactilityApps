#include "FTPServer.h"

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<FTPServer>();
    return 0;
}

}

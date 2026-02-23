#include "TodoList.h"
#include <TactilityCpp/App.h>

extern "C" {

int main(int argc, char* argv[]) {
    registerApp<TodoList>();
    return 0;
}

}

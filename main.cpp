#include "worldsim_app.h"
#include "memory_utils.h"

int main(int argc, char** argv) {
    configureProcessAllocatorForWorldSim();
    return runWorldSim3App(argc, argv);
}

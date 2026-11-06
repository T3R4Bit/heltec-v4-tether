#include "Tether.h"
#include "TetherModule.h"

void setupTether()
{
    tetherModule = new TetherModule();
    new TetherSniffer(*tetherModule); // registers itself with MeshModule; lives for the life of the firmware
}

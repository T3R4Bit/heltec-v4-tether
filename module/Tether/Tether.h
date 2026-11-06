// Entry point for Meshtastic's optional-module hook (bin/optional-modules.py looks for <Name>/<Name>.h and calls
// setup<Name>() from Modules.cpp via OPTIONAL_MODULES_SETUP). Keep this header minimal: it is included by Modules.cpp.
#pragma once

void setupTether();

// Unité de compilation dédiée à l'implémentation de dr_mp3 (single-header,
// domaine public / MIT-0). Garder DR_MP3_IMPLEMENTATION isolé ici évite de
// recompiler ce gros header à chaque modification de tts.cpp.
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

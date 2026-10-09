#pragma once
#include <stdint.h>
extern "C" int p2_game_rand();
extern "C" void p2_game_srand(uint32_t seed);
extern "C" uint32_t p2_game_rand_state();

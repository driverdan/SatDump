#include "free_space_path_loss.h"
#include <cmath>

FreeSpacePathLossAttenuation::FreeSpacePathLossAttenuation() : GenericAttenuation() {}

FreeSpacePathLossAttenuation::~FreeSpacePathLossAttenuation() {}

double FreeSpacePathLossAttenuation::get_attenuation() { return 92.45 + 20 * std::log10(slant_range * frequency); }
#include <obs-module.h>
#include "ai_matte_filter.hpp"

OBS_DECLARE_MODULE()

extern "C" MODULE_EXPORT bool obs_module_load(void)
{
	ai_matte_init_ort();
	obs_register_source(&ai_matte_filter_info);
	return true;
}

extern "C" MODULE_EXPORT void obs_module_unload(void)
{
	ai_matte_release_ort();
}

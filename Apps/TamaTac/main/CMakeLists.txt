file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE SFX_ENGINE_FILES ../../../Libraries/SfxEngine/Source/*.c*)

tactility_component_register(SRCS ${SOURCE_FILES} ${SFX_ENGINE_FILES} INCLUDE_DIRS ../../../Libraries/SfxEngine/Include)

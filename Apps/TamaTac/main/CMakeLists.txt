file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE SFX_ENGINE_FILES ../../../Libraries/SfxEngine/Source/*.c*)

idf_component_register(
    SRCS ${SOURCE_FILES} ${SFX_ENGINE_FILES}
    # Library headers must be included directly,
    # because all regular dependencies get stripped by elf_loader's cmake script
    INCLUDE_DIRS ../../../Libraries/TactilityCpp/Include ../../../Libraries/SfxEngine/Include
    REQUIRES TactilitySDK
)

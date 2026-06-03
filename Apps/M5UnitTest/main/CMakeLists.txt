file(GLOB_RECURSE SOURCE_FILES Source/*.c*)
file(GLOB_RECURSE UNIT_MODULE_FILES ../../../Libraries/M5UnitModules/Source/*.c*)

idf_component_register(
    SRCS ${SOURCE_FILES} ${UNIT_MODULE_FILES}
    # Library headers must be included directly,
    # because all regular dependencies get stripped by elf_loader's cmake script
    INCLUDE_DIRS ../../../Libraries/TactilityCpp/Include ../../../Libraries/M5UnitModules/Include
    REQUIRES TactilitySDK
)

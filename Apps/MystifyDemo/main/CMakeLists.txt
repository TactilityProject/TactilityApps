file(GLOB_RECURSE SOURCE_FILES Source/*.c*)

idf_component_register(
    SRC_DIRS "Source"
    # Library headers must be included directly,
    # because all regular dependencies get stripped by elf_loader's cmake script
    INCLUDE_DIRS "Include" "../../../Libraries/TactilityCpp/Include"
    REQUIRES TactilitySDK
)

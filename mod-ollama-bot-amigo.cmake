# Ensure the module is correctly registered before linking
if(TARGET modules)
    target_link_libraries(modules PRIVATE curl)

    # Ensure movement compilation unit is built
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotMovement.cpp)

    # World/physics helper compilation units
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Util/WorldChecks.cpp)

    # Travel semantics (completion/failure) unit
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotTravel.cpp)

    # Persistent memory (two-tier cache + DB backing)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Db/BotMemory.cpp)

    # Professions (execution-only)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotProfession.cpp)

    # Internal nav state (candidate_id -> engine destination)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotNavState.cpp)

    # Ensure module headers (including Bot/) are visible
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    
    # Explicitly include nlohmann-json path
    target_include_directories(modules PRIVATE /usr/local/include /usr/local/include/nlohmann)
endif()

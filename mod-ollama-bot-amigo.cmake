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

    # Bounded LLM worker pool shared by planner/control/chat.
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Ai/LlmDispatch.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Ai/BotMindState.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Script/AmigoSocial.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Script/AmigoCommands.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Script/AmigoAutoLogin.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Script/AmigoGroupAuthority.cpp)

    # Typed semantic missions + deterministic player-needs arbitration
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotMission.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotNeeds.cpp)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotLifecycle.cpp)

    # Recent in-memory history (movement outcomes + goal changes/completions)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotRecentHistory.cpp)

    # Internal nav state (candidate_id -> engine destination)
    target_sources(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src/Bot/BotNavState.cpp)

    # Ensure module headers (including Bot/) are visible
    target_include_directories(modules PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)
    
endif()

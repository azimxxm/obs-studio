cmake_minimum_required(VERSION 3.28...3.30)

# Live versions of the audio editor's voice effects, so a narrator can use them
# while recording too. Lives in its own folder so upstream merges never touch it.
add_library(narrator-filters MODULE)
add_library(OBS::narrator-filters ALIAS narrator-filters)

target_sources(narrator-filters PRIVATE narrator-filters.cpp)

target_link_libraries(narrator-filters PRIVATE OBS::libobs)

set_target_properties_obs(narrator-filters PROPERTIES FOLDER plugins PREFIX "")

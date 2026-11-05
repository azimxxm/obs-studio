# Narrator Studio: our own app name, bundle id and settings folder, so it can be
# installed next to the official OBS Studio without replacing it or sharing its
# settings. Kept in this one file so upstream merges stay easy.

set(NARRATOR_APP_NAME "Narrator Studio")
set(NARRATOR_BUNDLE_ID "uz.azimxxm.narrator-studio")
set(NARRATOR_CONFIG_DIR "narrator-studio")

target_compile_definitions(obs-studio PRIVATE NARRATOR_CONFIG_DIR="${NARRATOR_CONFIG_DIR}")

if(OS_MACOS)
  set_target_properties(obs-studio PROPERTIES OUTPUT_NAME "${NARRATOR_APP_NAME}")
  set_target_xcode_properties(
    obs-studio
    PROPERTIES PRODUCT_BUNDLE_IDENTIFIER "${NARRATOR_BUNDLE_ID}"
               PRODUCT_NAME "${NARRATOR_APP_NAME}"
               INFOPLIST_KEY_CFBundleDisplayName "${NARRATOR_APP_NAME}"
               INFOPLIST_KEY_NSMicrophoneUsageDescription
               "${NARRATOR_APP_NAME} records your voice from the microphone."
  )
endif()

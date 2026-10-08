set(QUOTE_JSON "${CMAKE_CURRENT_LIST_DIR}/../../quotes/quotes.json")
set(QUOTE_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/../../quotes/generate.py")
set(QUOTE_C "${CMAKE_CURRENT_BINARY_DIR}/quotes.c")

idf_component_register(
    SRCS "main.c" "ui.c" "telemetry.c" "audio.c" "${QUOTE_C}"
    INCLUDE_DIRS "."
)

idf_build_get_property(IDF_PYTHON PYTHON)
add_custom_command(
    OUTPUT "${QUOTE_C}"
    COMMAND "${IDF_PYTHON}" "${QUOTE_GENERATOR}" "${QUOTE_JSON}" --c-output "${QUOTE_C}"
    DEPENDS "${QUOTE_JSON}" "${QUOTE_GENERATOR}"
    VERBATIM
)

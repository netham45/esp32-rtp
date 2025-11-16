idf_component_register(SRC_DIRS .
                        REQUIRES cmock
                        INCLUDE_DIRS .
                        WHOLE_ARCHIVE)

# The following line would be needed to provide the 'main' function if this test used mocked FreeRTOS.
# As this test uses the real FreeRTOS implementation, we don't need Catch2 to provide 'main'.
#target_link_libraries(${COMPONENT_LIB} PRIVATE Catch2WithMain)

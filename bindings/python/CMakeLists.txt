find_package(Boost REQUIRED COMPONENTS python)
find_package(PythonLibs REQUIRED)
message(STATUS "Python includes are situated in (${PYTHON_INCLUDE_PATH}, ${PYTHON_INCLUDE_DIRS})")

include_directories(../../include)
include_directories(..)
include_directories(${PYTHON_INCLUDE_PATH})
include_directories(${PYTHON_INCLUDE_DIRS})
add_library(elliptics_python SHARED elliptics_python.cpp)
target_link_libraries(elliptics_python ${Boost_LIBRARIES} ${PYTHON_LIBRARIES} elliptics_cpp)
set_target_properties(elliptics_python PROPERTIES
    VERSION ${ELLIPTICS_VERSION}
    SOVERSION ${ELLIPTICS_VERSION_ABI}
    )

install(TARGETS elliptics_python
    LIBRARY DESTINATION lib${LIB_SUFFIX}
    ARCHIVE DESTINATION lib${LIB_SUFFIX}
    BUNDLE DESTINATION library
    )

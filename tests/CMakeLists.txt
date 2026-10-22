add_executable(tests
unit.cpp
integration.cpp
fuzz.cpp
fixtures.cpp
crash.cpp
)



target_link_directories(tests
PRIVATE

)
